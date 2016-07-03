#ifndef __LIBSCCMN_LOG_H__
#define __LIBSCCMN_LOG_H__

/*
Log levels:
  'T': trace
  'D': debug
  'I': info
  'W': warning
  'E': error
  'F': fatal
  'A': audit
*/

enum log_trace_ids
{
	L_TRACEID_SOCK_STREAM = 0x0001,
	L_TRACEID_EVENT_LOOP = 0x0002,
};

static inline bool _log_traceid(unsigned int traceid)
{
	return (libsccmn_config.log_trace_mask & traceid) != 0; 
}

void _log_v(const char level, const char * format, va_list args);
static inline void _log(const char level, const char * format, ...) __attribute__ ((__format__ (__printf__, 2, 3)));
static inline void _log(const char level, const char * format, ...)
{
	if ((level == 'D') && (!libsccmn_config.log_verbose)) return;

	va_list args;
	va_start(args, format);
	_log_v(level, format, args);
	va_end(args);
}

void _log_errno_v(int errnum, const char level, const char * format, va_list args);
static inline void _log_errno(int errnum, const char level, const char *format, ...) __attribute__((format(printf,3,4)));
static inline void _log_errno(int errnum, const char level, const char *format, ...)
{
	if ((level == 'D') && (!libsccmn_config.log_verbose)) return;

	va_list args;
	va_start(args, format);
	_log_errno_v(errnum, level, format, args);
	va_end(args);
}


void _log_openssl_err_v(char const level, const char * format, va_list args);
static inline void _log_openssl_err(char const level, const char * format, ...) __attribute__ ((__format__ (__printf__, 2, 3)));
static inline void _log_openssl_err(char const level, const char * format, ...)
{
	if ((level == 'D') && (!libsccmn_config.log_verbose)) return;

	va_list args;
	va_start(args, format);
	_log_openssl_err_v(level, format, args);
	va_end(args);
}

#define L_TRACE(traceid, fmt, args...) if (_log_traceid(traceid)) _log('T', "(%04X) %s:%s:%d " fmt, traceid, __FILE__, __func__, __LINE__, ## args)
#define L_DEBUG(fmt, args...) _log('D', fmt, ## args)
#define L_INFO(fmt, args...)  _log('I', fmt, ## args)
#define L_WARN(fmt, args...)  _log('W', fmt, ## args)
#define L_ERROR(fmt, args...) _log('E', fmt, ## args)
#define L_FATAL(fmt, args...) _log('F', fmt, ## args)
#define L_AUDIT(fmt, args...) _log('A', fmt, ## args)

#define L_DEBUG_P(fmt, args...) _log('D', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_INFO_P(fmt, args...)  _log('I', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_WARN_P(fmt, args...)  _log('W', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_ERROR_P(fmt, args...) _log('E', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_FATAL_P(fmt, args...) _log('F', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_AUDIT_P(fmt, args...) _log('A', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)

#define L_DEBUG_ERRNO(errnum, fmt, args...) _log_errno(errnum, 'D', fmt, ## args)
#define L_INFO_ERRNO(errnum, fmt, args...)  _log_errno(errnum, 'I', fmt, ## args)
#define L_WARN_ERRNO(errnum, fmt, args...)  _log_errno(errnum, 'W', fmt, ## args)
#define L_ERROR_ERRNO(errnum, fmt, args...) _log_errno(errnum, 'E', fmt, ## args)
#define L_FATAL_ERRNO(errnum, fmt, args...) _log_errno(errnum, 'F', fmt, ## args)
#define L_AUDIT_ERRNO(errnum, fmt, args...) _log_errno(errnum, 'A', fmt, ## args)

#define L_DEBUG_ERRNO_P(errnum, fmt, args...) _log_errno(errnum, 'D', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_INFO_ERRNO_P(errnum, fmt, args...)  _log_errno(errnum, 'I', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_WARN_ERRNO_P(errnum, fmt, args...)  _log_errno(errnum, 'W', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_ERROR_ERRNO_P(errnum, fmt, args...) _log_errno(errnum, 'E', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_FATAL_ERRNO_P(errnum, fmt, args...) _log_errno(errnum, 'F', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_AUDIT_ERRNO_P(errnum, fmt, args...) _log_errno(errnum, 'A', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)

#define L_INFO_OPENSSL(fmt, args...)  _log_openssl_err('I', fmt, ## args)
#define L_WARN_OPENSSL(fmt, args...)  _log_openssl_err('W', fmt, ## args)
#define L_ERROR_OPENSSL(fmt, args...) _log_openssl_err('E', fmt, ## args)
#define L_FATAL_OPENSSL(fmt, args...) _log_openssl_err('F', fmt, ## args)
#define L_AUDIT_OPENSSL(fmt, args...) _log_openssl_err('A', fmt, ## args)

#define L_INFO_OPENSSL_P(fmt, args...)  _log_openssl_err('I', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_WARN_OPENSSL_P(fmt, args...)  _log_openssl_err('W', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_ERROR_OPENSSL_P(fmt, args...) _log_openssl_err('E', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_FATAL_OPENSSL_P(fmt, args...) _log_openssl_err('F', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)
#define L_AUDIT_OPENSSL_P(fmt, args...) _log_openssl_err('A', "%s:%s:%d " fmt, __FILE__, __func__, __LINE__, ## args)

/*
 * Call this to open or reopen log file.
 * Used during initial switch from stderr a log file and eventually to rotate a log file
 */
bool logging_reopen(void);
void logging_finish(void);
void logging_flush(void);

void logging_set_context(struct context * context);

/*
 * Specify location of the log file.
 * Implicitly manages logging_reopen().
 * Can be called also with fname set to NULL to remove filename and switch back to stderr 
 */
bool logging_set_filename(const char * fname);

static inline bool logging_get_verbose(void)
{
	return libsccmn_config.log_verbose;
}

static inline void logging_set_verbose(bool v)
{
	libsccmn_config.log_verbose = v;
}

///

struct log_entry
{
	double timestamp;
	pid_t pid;
	char level;
	char message[4096];
};

void log_entry_process(struct log_entry * le, int le_message_length);

///

#endif //__LIBSCCMN_LOG_H__
