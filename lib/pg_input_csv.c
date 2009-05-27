/*
 * pg_bulkload: lib/pg_input_csv.c
 *
 *	  Copyright(C) 2007-2009, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 */

/**
 * @file
 * @brief CSV format file handling module implementation.
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup.h"
#include "utils/rel.h"

#include "pg_strutil.h"
#include "reader.h"

/**
 * @brief Initial size of the record buffer and the field buffer.
 *
 * If needed more, then the size will be doubled when needed.
 */
#define INITIAL_BUF_LEN (1024 * 1024)

typedef struct CSVParser
{
	Parser	base;

	/**
	 * @brief Record Buffer.
	 *
	 * This buffer stores the data read from the input file.
	 */
	char *rec_buf;
	
	/**
	 * @brief Field Buffer.
	 *
	 * This buffer stores character string representation of each field value,
	 * taken from the record buffer.   Quote marks and escapes have already
	 * developed.  Each field entry can be found in the link from "field".
	 */
	char *field_buf;
	
	/**
	 * @brief Contains the pointer to the character string for each field.
	 */
	char **fields;
	
	/**
	 * @brief Size of the record buffer and the field buffer.
	 * If needed more, then the size will be doubled when needed.
	 * Firld buffer size does not exceed a single record size.
	 * Therefore these two has the same size. These two buffers are
	 * expanded at the same time and therefore the size is contained
	 * in this single variable.
	 */
	int	buf_len;
	
	/**
	 * @brief Actual size in the record buffer, excluding the trailing NULL.
	 */
	int	used_len;
	
	/**
	 * @brief Pointer to the next record in the record buffer.
	 */
	char *next;
	
	/**
	 * @brief Flag indicating EOF has been encountered in the input file.
	 */
	bool eof;
	
	/**
	 * @brief String length for NULL string.
	 *
	 * strlen() This is actually cache variable to reduce function calls.
	 */
	int	null_len;

	char		delim;			/**< delimeter */
	char		quote;			/**< quotation string */
	char		escape;			/**< escape letter */
	char	   *null;			/**< NULL value string */
	List	   *fnn_name;		/**< list of NOT NULL column names */
	bool	   *fnn;			/**< array of NOT NULL column flag */
} CSVParser;

/*
 * Prototype declaration for local functions.
 */

static void	CSVParserInit(CSVParser *self, Reader *rd);
static bool	CSVParserRead(CSVParser *self, Reader *rd);
static void	CSVParserTerm(CSVParser *self);
static bool CSVParserParam(CSVParser *self, const char *keyword, char *value);

static void	ExtractValuesFromCSV(CSVParser *self, Reader *rd);

/*
 * @brief Copies specified area in the record buffer to the field buffer.
 *
 * This function must be called only when one of the following (non-loadable)
 * characters is found.
 * -# Open/Close quote character.
 * -# Valid escape character surrounded by quotation marks,
 * -# Delimiter character,
 * -# Record delimiter (new line, EOF).
 *
 * Flow
 * -# If non-zero lenght is specified, copies data and shift source/destination pointer.
 * -# Increment the source pointer to skip characters not to copy.
 *
 * @param dst [in/out] Copy destination address (field buffer index)
 * @param src [in/out] Copy destination (record buffer index)
 * @param len [in] Number of byte to copy
 * @return None
 */
static void
appendToField(CSVParser *self, int *dst, int *src, int len)
{
	if (len)
	{
		memcpy(self->field_buf + *dst, self->rec_buf + *src, len);
		*dst += len;
		*src += len;
		self->field_buf[*dst] = '\0';
	}
	/*
	 * Shift the source address for non-loading character.
	 */
	(*src)++;
}

/**
 * @brief Create a new CSV parser.
 */
Parser *
CreateCSVParser(void)
{
	CSVParser* self = palloc0(sizeof(CSVParser));
	self->base.init = (ParserInitProc) CSVParserInit;
	self->base.read = (ParserReadProc) CSVParserRead;
	self->base.term = (ParserTermProc) CSVParserTerm;
	self->base.param = (ParserParamProc) CSVParserParam;
	return (Parser *)self;
}

/**
 * @brief Initialize CSV file reader module.
 *
 * Flow
 * -# Allocate character string pointer array for valid columns.
 * -# Cache the length of NULL-value character string.
 *
 * @param rd [in] Control Info.
 * @return None
 * @note Caller must release the resource using CSVParserTerm().
 */
static void
CSVParserInit(CSVParser* self, Reader *rd)
{
	/*
	 * set default values
	 */
	self->delim = self->delim ? self->delim : ',';
	self->quote = self->quote ? self->quote : '"';
	self->escape = self->escape ? self->escape : '"';
	self->null = self->null ? self->null : "";

	/*
	 * validation check
	 */
	if (strchr(self->null, self->delim))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg
				 ("DELIMITER must not appear in the NULL specification")));
	if (strchr(self->null, self->quote))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg
				 ("QUOTE must not appear in the NULL specification")));

	/*
	 * set not NULL column information
	 */
	do
	{
		int			i;
		ListCell   *name;
		TupleDesc	tupDesc;

		tupDesc = RelationGetDescr(rd->ci_rel);
		self->fnn = palloc0(sizeof(bool) *rd->ci_attnumcnt);
		foreach(name, self->fnn_name)
		{
			for (i = 0; i < tupDesc->natts; i++)
			{
				if (strcmp(lfirst(name), tupDesc->attrs[i]->attname.data) == 0)
				{
					self->fnn[i] = true;
					break;
				}
			}
			/*
			 * if not exists, error
			 */
			if (i == tupDesc->natts)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
								errmsg("invalid column name [%s]",
									   (char *) lfirst(name))));
		}
	} while(0);

	/*
	 * XXX Although we would like to set INITIAL_BUF_LEN size to buffer length
	 * as initializing, only a half amount of memory has to be allocated here, 
	 * because we would like to avoid extra "if" check for the first time 
	 * allocation in loop(see "buflen *= 2" in this code). But this seems a 
	 * little bit ugly...
	 */
	self->buf_len = INITIAL_BUF_LEN / 2;
	self->rec_buf = palloc(self->buf_len);
	self->rec_buf[0] = '\0';
	self->used_len = 0;
	self->field_buf = palloc(self->buf_len);
	self->next = self->rec_buf;
	self->fields = palloc(rd->ci_attnumcnt * sizeof(char *));
	self->fields[0] = NULL;
	self->null_len = strlen(self->null);
	self->eof = false;

	/* Skip first ci_offset lines in the input file */
	if (rd->ci_offset > 0)
	{
#define LINEBUFLEN		1024
		char	buffer[LINEBUFLEN];
		int		len;
		int		skipped = 0;
		bool	inCR = false;

		while ((len = SourceRead(rd, buffer, LINEBUFLEN)) > 0)
		{
			int		i;

			for (i = 0; i < len; i++)
			{
				if (buffer[i] == '\r')
				{
					if (i == len - 1)
					{
						inCR = true;
						continue;
					}
					else if (buffer[i + 1] == '\n')
						i++;
				}
				else if (!inCR && buffer[i] != '\n')
					continue;

				/* Skip the line */
				inCR = false;
				++skipped;
				if (skipped >= rd->ci_offset)
				{
					/* Seek to head of the next line. */
					fseek(rd->ci_infd, i - len + 1, SEEK_CUR);
                    goto skip_done;
				}
			}
		}
		ereport(ERROR, (errcode_for_file_access(),
			errmsg("could not skip " int64_FMT " lines in the input file: %m",
				rd->ci_offset)));
skip_done:
		;	/* done */
	}
}

/**
 * @brief Release the resources used in CSV file reader module.
 *
 * Flow
 * -# Release the following resources,
 *	 - self->fields,
 *	 - self->rec_buf,
 *	 - self->field_buf.
 *
 * @param None
 * @return None
 */
static void
CSVParserTerm(CSVParser *self)
{
	if (self->fields)
		pfree(self->fields);
	if (self->rec_buf)
		pfree(self->rec_buf);
	if (self->field_buf)
		pfree(self->field_buf);
	pfree(self);
}

static bool
checkFieldIsNull(CSVParser *self, Reader *rd, int field_num, int len)
{
	/*
	 * We have to determine NULL value using character string before quote mark
	 * and escape character handling.	For this, we use the record buffer, not
	 * the field buffer (field buffer contains character string after these marks
	 * are handled).
	 */
	if (!self->fnn[rd->ci_attnumlist[field_num]] &&
		self->null_len == len &&
		0 == memcmp(self->null, self->fields[field_num], self->null_len))
	{
		self->fields[field_num] = NULL;
		return true;
	}
	else
		return false;
}

/**
 * @brief Reads one record from the input file, converts each field's
 * character string representation into PostgreSQL internal representation
 * and store it to the line data buffer.
 *
 * When this function is called, it is assumed that the context is in the
 * user-defined function context.	When returning from this function,
 * the memory context is switched to the tuple context.
 *
 * To cordinate the memory context in releasing memory, self->rec_buf and
 * self->field_buf are allocated only within this function.	Caller must
 * release these memory by releasing whole memory context.
 *
 * @param rd [in/out] Control Info.
 * @return Returns tru when successful, false when EOF is found.
 * @note When an error is found, it returns to the caller through ereport().
 */
static bool
CSVParserRead(CSVParser* self, Reader *rd)
{
	int			i = 0;			/* Index of the scanned character */
	int			ret;
	char		c;				/* Cache for the scanned character */
	char		quote = self->quote;	/* Cache for the quote mark */
	char		escape = self->escape; /* Cache for the escape character */
	char		delim = self->delim;	/* Cache for the delimiter */
	char	   *cur;			/* Return value */
	bool		need_data = false;		/* Flag indicating the need to read more characters */
	bool		in_quote = false;
	bool		inCR = false;

	/*
	 * Field parsing info
	 */
	int			field_head;		/* Index to the current field */
	int			dst;			/* Index to the next destination */
	int			src;			/* Index to the next source */
	int			field_num = 0;	/* Number of self->fields already parsed */
	int			fetched_num;

	/*
	 * If EOF found in the previous calls, returns zero.
	 */
	if (self->eof)
		return false;

	cur = self->next;

	/*
	 * Initialize variables related to fied data.
	 */
	src = cur - self->rec_buf;
	dst = 0;
	field_head = src;
	fetched_num = 1;
	self->field_buf[dst] = '\0';
	self->fields[field_num] = self->field_buf + dst;

	/*
	 * Loop for each input character to parse record buffer.
	 *
	 * Because errors are accumulated until specified numbers of erros are
	 * found, ereport() must no be used in this loop unless fatal error is found.
	 */
	for (i = cur - self->rec_buf;; i++)
	{
		/*
		 * If no record is found in the record buffer, read them from the input file.
		 */
		if (need_data)
		{
			/*
			 * When an escape character is found at the last of the buffer or no
			 * record delimiter is found in the record buffer, we extend the record
			 * buffer.
			 * - When the current line starts at the beginning of the record buffer,
			 *	 ->Buffer size is doubled and more data is read.
			 * - The current line is not at the begenning of the record buffer,
			 *	 ->Move the current line to the beginning of the record buffer and continue to read.
			 */
			if (cur != self->rec_buf)
			{
				int			move_size = cur - self->rec_buf;	/* Amount to move buffer. */

				memmove(self->rec_buf, cur, self->buf_len - move_size);
				self->used_len -= move_size;
				i -= move_size;
				field_head -= move_size;
				src -= move_size;
				cur = self->rec_buf;
			}
			else
			{
				int			j;
				char	   *old_buf = self->field_buf;

				self->buf_len *= 2;
				self->field_buf = repalloc(self->field_buf, self->buf_len);
				/*
				 * After repalloc(), address of each field needs to be adjusted.
				 */
				for (j = 0; j <= field_num; j++)
				{
					if (self->fields[j])
						self->fields[j] += self->field_buf - old_buf;
				}

				self->rec_buf = repalloc(self->rec_buf, self->buf_len);
				/*
				 * Expanded buffer may be different from the original one, so we reset the
				 * record beginning.
				 */
				cur = self->rec_buf;
			}

			ret = SourceRead(rd, self->rec_buf + self->used_len,
								self->buf_len - self->used_len - 1);
			if (ret == 0)
			{
				self->eof = true;
				/*
				 * When no data is found in the record buffer and we encounter EOF,
				 * there're no  more input to handle and return false.
				 */
				if (cur[0] == '\0')
					return false;

				/*
				 * When no corresponding (closing) quote mark is found and EOF is found,
				 * it's an error.   At this point, whole line has been parsed and exit from the loop.
				 */
				if (in_quote)
					break;

				/*
				 * To simplify the following parsing, when the last character of the input
				 * file is not new line code, we add this.
				 */
				if (self->rec_buf[i] == '\0')
				{
					ret++;
					self->rec_buf[i] = '\n';
				}
			}
			else if (ret < 0)
				ereport(ERROR, (errcode_for_file_access(),
								errmsg("could not read input file %m")));

			self->used_len += ret;
			self->rec_buf[self->used_len] = '\0';
			need_data = false;
		}

		c = self->rec_buf[i];
		if (c == '\0')
		{
			/*
			 * If parsing has been done upto the last of the buffer, we read next data.
			 */
			need_data = true;
			i--;				/* 'i--' is needed to cancel the incrementation in the for() loop definition. */
		}
		else if (in_quote)
		{
			/*
			 * Escape character must be followed by a quote mark or an excape character.
			 * If escape character is at the end of the buffer, data is read and then
			 * we test this.
			 */
			if (c == escape)
			{
				if (self->rec_buf[i + 1] == quote || self->rec_buf[i + 1] == escape)
				{
					appendToField(self, &dst, &src, i - src);
					i++;
				}
				else if (!self->rec_buf[i + 1])
				{
					need_data = true;
					i--;		/* 'i--' is needed here to cancel increment in for statement. */
				}
				else if (c == quote)
				{
					/*
					 * A quote mark will be identical to the escape character.	So if the
					 * following character is valid character and is not escaped, test
					 * again here if it is a quote mark.
					 */
					appendToField(self, &dst, &src, i - src);
					in_quote = false;
				}
			}
			else if (c == quote)
			{
				appendToField(self, &dst, &src, i - src);
				in_quote = false;
			}
		}
		else if (inCR)
		{
			appendToField(self, &dst, &src, i - src - 1);
			checkFieldIsNull(self, rd, field_num, i - field_head - 1);
			self->rec_buf[i - 1] = '\0';

			if (c != '\n')
				i--;	/* re-read the char */
			self->next = self->rec_buf + i + 1;

			inCR = false;
			break;
		}
		else
		{
			/*
			 * If a valid character is found at the begenning of the current line, the self->next
			 * record exists and increment the number of records to read.  The beginning
			 * of the record must not be a quote mark and we can test this here.
			 */
			if (i == cur - self->rec_buf)
				rd->ci_read_cnt++;

			if (c == quote)
			{
				appendToField(self, &dst, &src, i - src);
				in_quote = true;
			}
			else if (c == '\r')
			{
				inCR = true;
			}
			else if (c == '\n')
			{
				/*
				 * We determine the end of a field when a delimiter or line feed is found.
				 * Even if no line feed is found at the end of the input file, there will
				 * be no problem because we have already added line feed at EOF test above.
				 */
				appendToField(self, &dst, &src, i - src);

				checkFieldIsNull(self, rd, field_num, i - field_head);

				/*
				 * Line feed other than a quote mark is the record delimiter.  Record parse
				 * terminates when the record delmiter is found.
				 */
				self->rec_buf[i] = '\0';
				self->next = self->rec_buf + i + 1;
				break;
			}
			else if (c == delim)
			{
				appendToField(self, &dst, &src, i - src);

				checkFieldIsNull(self, rd, field_num, i - field_head);

				/*
				 * If then number of columns specified in the input record exceeds the
				 * number of columns of the copy target table, then the value of the last
				 * column of the table will be overwritten by extra columns in the input
				 * data successively.
				 */
				if (field_num + 1 < rd->ci_attnumcnt)
					field_num++;
				fetched_num++;

				/*
				 * The beginning of the next field is the next character from the delimiter.
				 */
				field_head = i + 1;
				/*
				 * Delmiter itself is not field data and skip this.
				 */
				dst++;
				/*
				 * Update the destination field
				 */
				self->field_buf[dst] = '\0';
				self->fields[field_num] = self->field_buf + dst;
			}
		}
	}

	/*
	 * If no corresponding (closing) quote mark is found when a record parse terminates, it's an error. 
	 */
	if (in_quote)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("unterminated CSV quoted field")));

	/*
	 * It's an error if the number of self->fields exceeds the number of valid column. 
	 */
	if (fetched_num > rd->ci_attnumcnt)
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("extra data after last expected column")));
	}
	/*
	 * Error, if the number of self->fields is less than the number of valid columns.
	 */
	if (fetched_num < rd->ci_attnumcnt)
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("missing data for column \"%s\"",
							   NameStr(RelationGetDescr(rd->ci_rel)->
									   attrs[rd->ci_attnumlist[fetched_num]]->
									   attname))));
	}

	ExtractValuesFromCSV(self, rd);

	return true;
}

static bool
CSVParserParam(CSVParser *self, const char *keyword, char *value)
{
	if (pg_strcasecmp(keyword, "DELIMITER") == 0)
	{
		ASSERT_ONCE(!self->delim);
		self->delim = ParseSingleChar(value);
	}
	else if (pg_strcasecmp(keyword, "QUOTE") == 0)
	{
		ASSERT_ONCE(!self->quote);
		self->quote = ParseSingleChar(value);
	}
	else if (pg_strcasecmp(keyword, "ESCAPE") == 0)
	{
		ASSERT_ONCE(!self->escape);
		self->escape = ParseSingleChar(value);
	}
	else if (pg_strcasecmp(keyword, "NULL") == 0)
	{
		ASSERT_ONCE(!self->null);
		self->null = pstrdup(value);
	}
	else if (pg_strcasecmp(keyword, "FORCE_NOT_NULL") == 0)
	{
		self->fnn_name = lappend(self->fnn_name, pstrdup(value));
	}
	else
		return false;	/* unknown parameter */

	return true;
}

/**
 * @brief Obtain an internal representation of each column from field array data for a record.
 *
 * Flow
 * -# For each field array member, repeat the following.
 *	 <dl>
 *	   <dt>When either FORCE_NOT_NULL is specified or the address stored in the field array
 *		   is not NULL,</dt>
 *		 <dd>Convert the field character string into the internal representation and set
 *			 NULL value marker to false. </dd>
 *	   <dt>When the address stored in the field array is NULL,</dt>
 *		 <dd>Set NULL value marker to true.</dd>
 *	 </dl>
 *
 * This function converts each field string into corresponding internal
 * representation and stores in rd->ci_values and rd->ci_isnull.
 *
 * @param rd [in/out] Control Info.
 * @return None
 * @note Memory areas allocated in this fuhnction cannot be rleased one by onek.  So the
 * caller of this function must set the memory context which allows to reset or discard
 * them.   When an error occurs, this returns to the caller using ereport().
 * @note When error occurs, return to the caller with ereport().
 */
static void
ExtractValuesFromCSV(CSVParser *self, Reader *rd)
{
	int					i;
	Form_pg_attribute  *attrs = RelationGetDescr(rd->ci_rel)->attrs;

	/*
	 * Converts string data in the field array into the internal representation for
	 * a destination column.
	 */
	for (i = 0; i < rd->ci_attnumcnt; i++)
	{
		int		index;

		rd->ci_parsing_field = i + 1;		/* ci_parsing_field is 1 origin */

		index = rd->ci_attnumlist[i];	/* Physical column index */
		if (self->fields[i] || self->fnn[index])
		{
			rd->ci_isnull[index] = false;
			rd->ci_values[index] = FunctionCall3(&rd->ci_in_functions[index],
				CStringGetDatum(self->fields[i]),
				ObjectIdGetDatum(rd->ci_typeioparams[index]),
				Int32GetDatum(attrs[index]->atttypmod));
		}
		else
		{
			if (attrs[index]->attnotnull)
			{
				/*
				 * If the input is NULL value, check NON NULL constraint for this table.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg
						 ("null value in column \"%s\" violates not-null constraint",
						  NameStr(attrs[index]->attname))));
			}
			rd->ci_isnull[index] = true;
		}
	}
}