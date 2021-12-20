/*****************************************************************************

Copyright (c) 1994, 2021, Oracle and/or its affiliates.
Copyright (c) 2008, Google Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/***********************************************************************/ /**
 @file include/univ.i
 Version control for database, common definitions, and include files

 Created 1/20/1994 Heikki Tuuri
 ****************************************************************************/

#ifndef univ_i
#define univ_i

#ifdef UNIV_HOTBACKUP
#include "hb_univ.i"
#endif /* UNIV_HOTBACKUP */

/* aux macros to convert M into "123" (string) if M is defined like
#define M 123 */
#define _IB_TO_STR(s) #s
#define IB_TO_STR(s) _IB_TO_STR(s)

#define INNODB_VERSION_MAJOR MYSQL_VERSION_MAJOR
#define INNODB_VERSION_MINOR MYSQL_VERSION_MINOR
#define INNODB_VERSION_BUGFIX MYSQL_VERSION_PATCH

/* The following is the InnoDB version as shown in
SELECT plugin_version FROM information_schema.plugins;
calculated in make_version_string() in sql/sql_show.cc like this:
"version >> 8" . "version & 0xff"
because the version is shown with only one dot, we skip the last
component, i.e. we show M.N.P as M.N */
#define INNODB_VERSION_SHORT (INNODB_VERSION_MAJOR << 8 | INNODB_VERSION_MINOR)

#define INNODB_VERSION_STR        \
  IB_TO_STR(INNODB_VERSION_MAJOR) \
  "." IB_TO_STR(INNODB_VERSION_MINOR) "." IB_TO_STR(INNODB_VERSION_BUGFIX)

#define REFMAN                                  \
  "http://dev.mysql.com/doc/refman/" IB_TO_STR( \
      MYSQL_VERSION_MAJOR) "." IB_TO_STR(MYSQL_VERSION_MINOR) "/en/"

#ifdef MYSQL_DYNAMIC_PLUGIN
/* In the dynamic plugin, redefine some externally visible symbols
in order not to conflict with the symbols of a builtin InnoDB. */

/* Rename all C++ classes that contain virtual functions, because we
have not figured out how to apply the visibility=hidden attribute to
the virtual method table (vtable) in GCC 3. */
#define ha_innobase ha_innodb
#endif /* MYSQL_DYNAMIC_PLUGIN */

#if defined(_WIN32)
#include <windows.h>
#endif /* _WIN32 */

/* The defines used with MySQL */

/* Include a minimum number of SQL header files so that few changes
made in SQL code cause a complete InnoDB rebuild.  These headers are
used throughout InnoDB but do not include too much themselves.  They
support cross-platform development and expose commonly used SQL names. */

#include "m_string.h"
#ifndef UNIV_HOTBACKUP
#include "my_thread.h"
#endif /* !UNIV_HOTBACKUP  */

#include <limits>
/* Include <sys/stat.h> to get S_I... macros defined for os0file.cc */
#include <sys/stat.h>

#include "my_psi_config.h"

#ifndef _WIN32
#include <sched.h>
#include <sys/mman.h> /* mmap() for os0proc.cc */
#endif                /* !_WIN32 */

/* Include the header file generated by CMake */
#ifndef _WIN32
#ifndef UNIV_HOTBACKUP
#include "my_config.h"
#endif /* !UNIV_HOTBACKUP */
#endif

#ifndef UNIV_HOTBACKUP
#include <inttypes.h>
#include <stdint.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#endif /* !UNIV_HOTBACKUP */

/* Following defines are to enable performance schema
instrumentation in each of five InnoDB modules if
HAVE_PSI_INTERFACE is defined. */
#if defined(HAVE_PSI_INTERFACE) && !defined(UNIV_LIBRARY)
#ifdef HAVE_PSI_MUTEX_INTERFACE
#define UNIV_PFS_MUTEX
#endif /* HAVE_PSI_MUTEX_INTERFACE */

#if defined HAVE_PSI_RWLOCK_INTERFACE && defined UNIV_PFS_MUTEX
/* For the rwlocks to be tracked UNIV_PFS_MUTEX has to be defined. If not
defined, the rwlocks are simply not tracked. */
#define UNIV_PFS_RWLOCK
#endif /* HAVE_PSI_RWLOCK_INTERFACE */

#ifdef HAVE_PSI_FILE_INTERFACE
#define UNIV_PFS_IO
#endif /* HAVE_PSI_FILE_INTERFACE */

#ifdef HAVE_PSI_THREAD_INTERFACE
#define UNIV_PFS_THREAD
#endif /* HAVE_PSI_THREAD_INTERFACE */

#ifdef HAVE_PSI_MEMORY_INTERFACE
#define UNIV_PFS_MEMORY
#endif /* HAVE_PSI_MEMORY_INTERFACE */

#include "mysql/psi/mysql_file.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_rwlock.h"
#include "mysql/psi/mysql_thread.h"
/* For PSI_FILE_CALL(). */
#include "pfs_file_provider.h"
#include "pfs_mutex_provider.h"
#include "pfs_rwlock_provider.h"
/* For PSI_MUTEX_CALL() and similar. */
#include "pfs_thread_provider.h"

#endif /* HAVE_PSI_INTERFACE && !UNIV_LIBRARY */

#ifdef _WIN32
#define YY_NO_UNISTD_H 1
/* VC++ tries to optimise for size by default, from V8+. The size of
the pointer to member depends on whether the type is defined before the
compiler sees the type in the translation unit. This default behaviour
can cause the pointer to be a different size in different translation
units, depending on the above rule. We force optimise for size behaviour
for all cases. This is used by ut0lst.h related code. */
#pragma pointers_to_members(full_generality, single_inheritance)
#endif /* _WIN32 */

/*			DEBUG VERSION CONTROL
                        ===================== */

/* When this macro is defined then additional test functions will be
compiled. These functions live at the end of each relevant source file
and have "test_" prefix. These functions can be called from the end of
innobase_init() or they can be called from gdb after
innobase_start_or_create_for_mysql() has executed using the call
command. */
/*
#define UNIV_COMPILE_TEST_FUNCS
#define UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
#define UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
#define UNIV_ENABLE_UNIT_TEST_DICT_STATS
#define UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT
*/

#if defined HAVE_VALGRIND
#define UNIV_DEBUG_VALGRIND
#endif /* HAVE_VALGRIND */

#ifdef NDEBUG
#undef UNIV_DEBUG
#elif !defined UNIV_DEBUG
#define UNIV_DEBUG
#endif

#if 0
#define UNIV_DEBUG_VALGRIND        /* Enable extra \
                                   Valgrind instrumentation */
#define UNIV_DEBUG_PRINT           /* Enable the compilation of \
                                   some debug print functions */
#define UNIV_AHI_DEBUG             /* Enable adaptive hash index \
                                   debugging without UNIV_DEBUG */
#define UNIV_BUF_DEBUG             /* Enable buffer pool \
                                   debugging without UNIV_DEBUG */
#define UNIV_BLOB_LIGHT_DEBUG      /* Enable off-page column \
                                   debugging without UNIV_DEBUG */
#define UNIV_DEBUG_LOCK_VALIDATE   /* Enable                       \
                                   ut_ad(lock_rec_validate_page()) \
                                   assertions. */
#define UNIV_LRU_DEBUG             /* debug the buffer pool LRU */
#define UNIV_HASH_DEBUG            /* debug HASH_ macros */
#define UNIV_LOG_LSN_DEBUG         /* write LSN to the redo log;               \
this will break redo log file compatibility, but it may be useful when \
debugging redo log application problems. */
#define UNIV_IBUF_DEBUG            /* debug the insert buffer */
#define UNIV_IBUF_COUNT_DEBUG      /* debug the insert buffer;               \
this limits the database to IBUF_COUNT_N_SPACES and IBUF_COUNT_N_PAGES, \
and the insert buffer must be empty when the database is started */
#define UNIV_PERF_DEBUG            /* debug flag that enables \
                                   light weight performance   \
                                   related stuff. */
#define UNIV_SEARCH_PERF_STAT      /* statistics for the \
                                   adaptive hash index */
#define UNIV_SRV_PRINT_LATCH_WAITS /* enable diagnostic output \
                                   in sync0sync.cc */
#define UNIV_BTR_PRINT             /* enable functions for \
                                   printing B-trees */
#define UNIV_ZIP_DEBUG             /* extensive consistency checks \
                                   for compressed pages */
#define UNIV_ZIP_COPY              /* call page_zip_copy_recs() \
                                   more often */
#define UNIV_AIO_DEBUG             /* prints info about     \
                                   submitted and reaped AIO \
                                   requests to the log. */
#define UNIV_STATS_DEBUG           /* prints various stats \
                                   related debug info from \
                                   dict0stats.c */
#define FTS_INTERNAL_DIAG_PRINT    /* FTS internal debugging \
                                   info output */
#define UNIV_DEBUG_DEDICATED       /* dedicated server debugging \
                                   info output and code coverage */
#endif

#define UNIV_BTR_DEBUG       /* check B-tree links */
#define UNIV_LIGHT_MEM_DEBUG /* light memory debugging */

// #define UNIV_SQL_DEBUG

#if defined(COMPILER_HINTS) && defined __GNUC__ && \
    (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 3)

/** Starting with GCC 4.3, the "cold" attribute is used to inform the
compiler that a function is unlikely executed.  The function is
optimized for size rather than speed and on many targets it is placed
into special subsection of the text section so all cold functions
appears close together improving code locality of non-cold parts of
program.  The paths leading to call of cold functions within code are
marked as unlikely by the branch prediction mechanism.  optimize a
rarely invoked function for size instead for speed. */
#define UNIV_COLD MY_ATTRIBUTE((cold))
#else
#define UNIV_COLD /* empty */
#endif

#ifdef _WIN32
#ifdef _WIN64
constexpr size_t UNIV_WORD_SIZE = 8;
#else
constexpr size_t UNIV_WORD_SIZE = 4;
#endif
#else  /* !_WIN32 */
/** MySQL config.h generated by CMake will define SIZEOF_LONG in Posix */
constexpr size_t UNIV_WORD_SIZE = SIZEOF_LONG;
#endif /* _WIN32 */

/** The following alignment is used in memory allocations in memory heap
management to ensure correct alignment for doubles etc. */
constexpr uint32_t UNIV_MEM_ALIGNMENT = 8;

/*
                        DATABASE VERSION CONTROL
                        ========================
*/

/** The 2-logarithm of UNIV_PAGE_SIZE: */
#define UNIV_PAGE_SIZE_SHIFT srv_page_size_shift

/** The universal page size of the database */
#define UNIV_PAGE_SIZE ((ulint)srv_page_size)

/** log2 of smallest compressed page size (1<<10 == 1024 bytes)
Note: This must never change! */
constexpr uint32_t UNIV_ZIP_SIZE_SHIFT_MIN = 10;

/** log2 of largest compressed page size (1<<14 == 16384 bytes).
A compressed page directory entry reserves 14 bits for the start offset
and 2 bits for flags. This limits the uncompressed page size to 16k.
Even though a 16k uncompressed page can theoretically be compressed
into a larger compressed page, it is not a useful feature so we will
limit both with this same constant. */
constexpr uint32_t UNIV_ZIP_SIZE_SHIFT_MAX = 14;

/* Define the Min, Max, Default page sizes. */
/** Minimum Page Size Shift (power of 2) */
constexpr uint32_t UNIV_PAGE_SIZE_SHIFT_MIN = 12;
/** Maximum Page Size Shift (power of 2) */
constexpr uint32_t UNIV_PAGE_SIZE_SHIFT_MAX = 16;
/** Default Page Size Shift (power of 2) */
constexpr uint32_t UNIV_PAGE_SIZE_SHIFT_DEF = 14;
/** Original 16k InnoDB Page Size Shift, in case the default changes */
constexpr uint32_t UNIV_PAGE_SIZE_SHIFT_ORIG = 14;
/** Original 16k InnoDB Page Size as an ssize (log2 - 9) */
constexpr uint32_t UNIV_PAGE_SSIZE_ORIG = UNIV_PAGE_SIZE_SHIFT_ORIG - 9;

/** Minimum page size InnoDB currently supports. */
constexpr uint32_t UNIV_PAGE_SIZE_MIN = 1 << UNIV_PAGE_SIZE_SHIFT_MIN;
/** Maximum page size InnoDB currently supports. */
constexpr size_t UNIV_PAGE_SIZE_MAX = 1 << UNIV_PAGE_SIZE_SHIFT_MAX;
/** Default page size for InnoDB tablespaces. */
constexpr uint32_t UNIV_PAGE_SIZE_DEF = 1 << UNIV_PAGE_SIZE_SHIFT_DEF;
/** Original 16k page size for InnoDB tablespaces. */
constexpr uint32_t UNIV_PAGE_SIZE_ORIG = 1 << UNIV_PAGE_SIZE_SHIFT_ORIG;

/** Smallest compressed page size */
constexpr uint32_t UNIV_ZIP_SIZE_MIN = 1 << UNIV_ZIP_SIZE_SHIFT_MIN;

/** Largest compressed page size */
constexpr uint32_t UNIV_ZIP_SIZE_MAX = 1 << UNIV_ZIP_SIZE_SHIFT_MAX;

/** Largest possible ssize for an uncompressed page.
(The convention 'ssize' is used for 'log2 minus 9' or the number of
shifts starting with 512.)
This max number varies depending on UNIV_PAGE_SIZE. */
#define UNIV_PAGE_SSIZE_MAX \
  static_cast<ulint>(UNIV_PAGE_SIZE_SHIFT - UNIV_ZIP_SIZE_SHIFT_MIN + 1)

/** Smallest possible ssize for an uncompressed page. */
#define UNIV_PAGE_SSIZE_MIN \
  static_cast<ulint>(UNIV_PAGE_SIZE_SHIFT_MIN - UNIV_ZIP_SIZE_SHIFT_MIN + 1)

/** Maximum number of parallel threads in a parallelized operation */
constexpr uint32_t UNIV_MAX_PARALLELISM = 32;

/** This is the "mbmaxlen" for my_charset_filename (defined in
strings/ctype-utf8.c), which is used to encode File and Database names. */
constexpr uint32_t FILENAME_CHARSET_MAXNAMLEN = 5;

/** The maximum length of an encode table name in bytes.  The max
table and database names are NAME_CHAR_LEN (64) characters. After the
encoding, the max length would be NAME_CHAR_LEN (64) *
FILENAME_CHARSET_MAXNAMLEN (5) = 320 bytes. The number does not include a
terminating '\0'. InnoDB can handle longer names internally */
constexpr uint32_t MAX_TABLE_NAME_LEN = 320;

/** The maximum length of a database name. Like MAX_TABLE_NAME_LEN this is
the MySQL's NAME_LEN, see check_and_convert_db_name(). */
constexpr uint32_t MAX_DATABASE_NAME_LEN = MAX_TABLE_NAME_LEN;

/** MAX_FULL_NAME_LEN defines the full name path including the
database name and table name. In addition, 14 bytes is added for:
        2 for surrounding quotes around table name
        1 for the separating dot (.)
        9 for the #mysql50# prefix */
constexpr uint32_t MAX_FULL_NAME_LEN =
    MAX_TABLE_NAME_LEN + MAX_DATABASE_NAME_LEN + 14;

/** Maximum length of the compression algorithm string. Currently we support
only (NONE | ZLIB | LZ4). */
constexpr uint32_t MAX_COMPRESSION_LEN = 4;

/*
                        UNIVERSAL TYPE DEFINITIONS
                        ==========================
*/

/* Note that inside MySQL 'byte' is defined as char on Linux! */
using byte = unsigned char;

/* Another basic type we use is unsigned long integer which should be equal to
the word size of the machine, that is on a 32-bit platform 32 bits, and on a
64-bit platform 64 bits. We also give the printf format for the type as a
macro ULINTPF. We also give the printf format suffix (without '%') macro
ULINTPFS, this one can be useful if we want to put something between % and
lu/llu, like in %03lu. */

/* Use the integer types and formatting strings defined in the C++11 standard.
 */
#define UINT16PF "%" PRIu16
#define UINT32PF "%" PRIu32
#define UINT32PFS PRIu32
#define UINT64PF "%" PRIu64
#define UINT64PFx "%016" PRIx64

#define IB_ID_FMT UINT64PF

#ifdef _WIN64
typedef unsigned __int64 ulint;
typedef __int64 lint;
#define ULINTPFS "llu"
#else
typedef unsigned long int ulint;
typedef long int lint;
#define ULINTPFS "lu"
#endif /* _WIN64 */

#define ULINTPF "%" ULINTPFS

#ifndef _WIN32
#if SIZEOF_LONG != SIZEOF_VOIDP
#error "Error: InnoDB's ulint must be of the same size as void*"
#endif
#endif

/** The 'undefined' value for a ulint */
constexpr ulint ULINT_UNDEFINED = ~ulint{0U};

constexpr ulong ULONG_UNDEFINED = ~0UL;

/** The 'undefined' value for a  64-bit unsigned integer */
constexpr uint64_t UINT64_UNDEFINED = ~0ULL;

/** The 'undefined' value for a  32-bit unsigned integer */
constexpr uint32_t UINT32_UNDEFINED = ~0U;

/** The 'undefined' value for a 16-bit unsigned integer */
constexpr uint16_t UINT16_UNDEFINED = std::numeric_limits<uint16_t>::max();

/** The 'undefined' value for a 8-bit unsigned integer */
constexpr uint8_t UINT8_UNDEFINED = std::numeric_limits<uint8_t>::max();

/** The bitmask of 32-bit unsigned integer */
constexpr uint32_t UINT32_MASK = 0xFFFFFFFF;

/** Maximum value for a ulint */
constexpr ulint ULINT_MAX = std::numeric_limits<ulint>::max() - 1;

/** The generic InnoDB system object identifier data type */
typedef uint64_t ib_id_t;
constexpr ib_id_t IB_ID_MAX = std::numeric_limits<uint64_t>::max();

/** Page number */
typedef uint32_t page_no_t;
/** Tablespace identifier */
typedef uint32_t space_id_t;

#define SPACE_ID_PF UINT32PF
#define SPACE_ID_PFS UINT32PFS
#define PAGE_NO_PF UINT32PF
#define PAGE_ID_PF "page " SPACE_ID_PF ":" PAGE_NO_PF

#define UNIV_NOTHROW

/** The following number as the length of a logical field means that the field
has the SQL NULL as its value. NOTE that because we assume that the length
of a field is a 32-bit integer when we store it, for example, to an undo log
on disk, we must have also this number fit in 32 bits, also in 64-bit
computers! */
constexpr uint32_t UNIV_SQL_NULL = UINT32_UNDEFINED;

/** Flag to indicate a field which was added instantly */
constexpr auto UNIV_SQL_ADD_COL_DEFAULT = UNIV_SQL_NULL - 1;

/** The following number as the length of a logical field means that no
attribute value for the multi-value index exists in the JSON doc */
constexpr auto UNIV_NO_INDEX_VALUE = UNIV_SQL_ADD_COL_DEFAULT - 1;

/** The following number as the length marker of a logical field, which
is only used for multi-value field data, means the data itself of the
field is actually an array. */
const uint32_t UNIV_MULTI_VALUE_ARRAY_MARKER = UNIV_NO_INDEX_VALUE - 1;

/** Flag to indicate a field which was dropped instantly */
constexpr auto UNIV_SQL_INSTANT_DROP_COL = UNIV_MULTI_VALUE_ARRAY_MARKER - 1;

/** Lengths which are not UNIV_SQL_NULL, but bigger than the following
number indicate that a field contains a reference to an externally
stored part of the field in the tablespace. The length field then
contains the sum of the following flag and the locally stored len. */

constexpr uint32_t UNIV_EXTERN_STORAGE_FIELD =
    UNIV_SQL_NULL - UNIV_PAGE_SIZE_DEF;

/* Some macros to improve branch prediction and reduce cache misses */
#if defined(COMPILER_HINTS) && defined(__GNUC__)
/* Tell the compiler that 'expr' probably evaluates to 'constant'. */
#define UNIV_EXPECT(expr, constant) __builtin_expect(expr, constant)
/* Tell the compiler that a pointer is likely to be NULL */
#define UNIV_LIKELY_NULL(ptr) __builtin_expect((ulint)ptr, 0)
/* Minimize cache-miss latency by moving data at addr into a cache before
it is read. */
#define UNIV_PREFETCH_R(addr) __builtin_prefetch(addr, 0, 3)
/* Minimize cache-miss latency by moving data at addr into a cache before
it is read or written. */
#define UNIV_PREFETCH_RW(addr) __builtin_prefetch(addr, 1, 3)

#elif defined __WIN__ && defined COMPILER_HINTS
#include <xmmintrin.h>

#define UNIV_EXPECT(expr, value) (expr)
#define UNIV_LIKELY_NULL(expr) (expr)
// __MM_HINT_T0 - (temporal data)
// prefetch data into all levels of the cache hierarchy.
#define UNIV_PREFETCH_R(addr) _mm_prefetch((char *)addr, _MM_HINT_T0)
#define UNIV_PREFETCH_RW(addr) _mm_prefetch((char *)addr, _MM_HINT_T0)
#else
/* Dummy versions of the macros */
#define UNIV_EXPECT(expr, value) expr
#define UNIV_LIKELY_NULL(expr) expr
#define UNIV_PREFETCH_R(addr) ((void)0)
#define UNIV_PREFETCH_RW(addr) ((void)0)
#endif

/* Tell the compiler that cond is likely to hold */
#define UNIV_LIKELY(cond) UNIV_EXPECT(cond, true)
/* Tell the compiler that cond is unlikely to hold */
#define UNIV_UNLIKELY(cond) UNIV_EXPECT(cond, false)

/* Compile-time constant of the given array's size. */
#define UT_ARR_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The return type from a thread's start function differs between Unix and
Windows, so define a typedef for it and a macro to use at the end of such
functions. */

#ifdef _WIN32
typedef ulint os_thread_ret_t;
#define OS_PATH_SEPARATOR_STR "\\"
#define OS_PATH_SEPARATOR '\\'
#define OS_PATH_SEPARATOR_ALT '/'
#else
typedef void *os_thread_ret_t;
#define OS_PATH_SEPARATOR_STR "/"
#define OS_PATH_SEPARATOR '/'
#define OS_PATH_SEPARATOR_ALT '\\'
#endif /* _WIN32 */

#include <stdio.h>

#include "db0err.h"
#include "sync0types.h"
#include "ut0dbg.h"
#include "ut0lst.h"
#include "ut0ut.h"

#ifdef UNIV_DEBUG_VALGRIND
#include <valgrind/memcheck.h>

#define UNIV_MEM_VALID(addr, size) VALGRIND_MAKE_MEM_DEFINED(addr, size)
#define UNIV_MEM_INVALID(addr, size) VALGRIND_MAKE_MEM_UNDEFINED(addr, size)
#define UNIV_MEM_FREE(addr, size) VALGRIND_MAKE_MEM_NOACCESS(addr, size)
#define UNIV_MEM_ALLOC(addr, size) VALGRIND_MAKE_MEM_UNDEFINED(addr, size)
#define UNIV_MEM_DESC(addr, size) VALGRIND_CREATE_BLOCK(addr, size, #addr)
#define UNIV_MEM_UNDESC(b) VALGRIND_DISCARD(b)
#define UNIV_MEM_ASSERT_RW_LOW(addr, size, should_abort)                      \
  do {                                                                        \
    const void *_p =                                                          \
        (const void *)(ulint)VALGRIND_CHECK_MEM_IS_DEFINED(addr, size);       \
    if (UNIV_LIKELY_NULL(_p)) {                                               \
      fprintf(stderr, "%s:%d: %p[%u] undefined at %ld\n", __FILE__, __LINE__, \
              (const void *)(addr), (unsigned)(size),                         \
              (long)(((const char *)_p) - ((const char *)(addr))));           \
      if (should_abort) {                                                     \
        ut_error;                                                             \
      }                                                                       \
    }                                                                         \
  } while (0)
#define UNIV_MEM_ASSERT_RW(addr, size) UNIV_MEM_ASSERT_RW_LOW(addr, size, false)
#define UNIV_MEM_ASSERT_RW_ABORT(addr, size) \
  UNIV_MEM_ASSERT_RW_LOW(addr, size, true)
#define UNIV_MEM_ASSERT_W(addr, size)                                          \
  do {                                                                         \
    const void *_p =                                                           \
        (const void *)(ulint)VALGRIND_CHECK_MEM_IS_ADDRESSABLE(addr, size);    \
    if (UNIV_LIKELY_NULL(_p))                                                  \
      fprintf(stderr, "%s:%d: %p[%u] unwritable at %ld\n", __FILE__, __LINE__, \
              (const void *)(addr), (unsigned)(size),                          \
              (long)(((const char *)_p) - ((const char *)(addr))));            \
  } while (0)
#define UNIV_MEM_TRASH(addr, c, size) \
  do {                                \
    void *p = (addr);                 \
    ut_d(memset(p, c, size));         \
    UNIV_MEM_INVALID(addr, size);     \
  } while (0)
#else
#define UNIV_MEM_VALID(addr, size) \
  do {                             \
  } while (0)
#define UNIV_MEM_INVALID(addr, size) \
  do {                               \
  } while (0)
#define UNIV_MEM_FREE(addr, size) \
  do {                            \
  } while (0)
#define UNIV_MEM_ALLOC(addr, size) \
  do {                             \
  } while (0)
#define UNIV_MEM_DESC(addr, size) \
  do {                            \
  } while (0)
#define UNIV_MEM_UNDESC(b) \
  do {                     \
  } while (0)
#define UNIV_MEM_ASSERT_RW_LOW(addr, size, should_abort) \
  do {                                                   \
  } while (0)
#define UNIV_MEM_ASSERT_RW(addr, size) \
  do {                                 \
  } while (0)
#define UNIV_MEM_ASSERT_RW_ABORT(addr, size) \
  do {                                       \
  } while (0)
#define UNIV_MEM_ASSERT_W(addr, size) \
  do {                                \
  } while (0)
#define UNIV_MEM_TRASH(addr, c, size) \
  do {                                \
  } while (0)
#endif
#define UNIV_MEM_ASSERT_AND_FREE(addr, size) \
  do {                                       \
    UNIV_MEM_ASSERT_W(addr, size);           \
    UNIV_MEM_FREE(addr, size);               \
  } while (0)
#define UNIV_MEM_ASSERT_AND_ALLOC(addr, size) \
  do {                                        \
    UNIV_MEM_ASSERT_W(addr, size);            \
    UNIV_MEM_ALLOC(addr, size);               \
  } while (0)

extern ulong srv_page_size_shift;
extern ulong srv_page_size;

static const size_t UNIV_SECTOR_SIZE = 512;

/* Dimension of spatial object we support so far. It has its root in
myisam/sp_defs.h. We only support 2 dimension data */
constexpr uint32_t SPDIMS = 2;

/** Hard-coded data dictionary entry */
#define INNODB_DD_TABLE(name, n_indexes) \
  { name, n_indexes }

/** Explicitly call the destructor, this is to get around Clang bug#12350.
@param[in,out]	p		Instance on which to call the destructor */
template <typename T>
void call_destructor(T *p) {
  p->~T();
}

template <typename T>
constexpr auto to_int(T v) -> typename std::underlying_type<T>::type {
  return (static_cast<typename std::underlying_type<T>::type>(v));
}

/** If we are doing something that takes longer than this many seconds then
print an informative message. */
static constexpr std::chrono::seconds PRINT_INTERVAL{10};

#if defined(UNIV_LIBRARY) && !defined(UNIV_NO_ERR_MSGS)

/** Don't use the server logging infrastructure if building
as a standalone library. */
#define UNIV_NO_ERR_MSGS

#endif /* UNIV_LIBRARY && !UNIV_NO_ERR_MSGS */

#ifdef UNIV_DEBUG
#define IF_DEBUG(...) __VA_ARGS__
#define IF_ENABLED(s, ...)        \
  if (Sync_point::enabled((s))) { \
    __VA_ARGS__                   \
  }
#else

/* Expand the macro if we are generating Doxygen documentation. */
#ifdef DOXYGEN_IF_DEBUG
#define IF_DEBUG(...) __VA_ARGS__
#else
#define IF_DEBUG(...)
#endif /* DOXYGEN_IF_DEBUG */

#define IF_ENABLED(s, ...)
#endif /* UNIV_DEBUG */

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
#define IF_AHI_DEBUG(...) __VA_ARGS__
#else /* UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#define IF_AHI_DEBUG(...)
#endif /* UNIV_AHI_DEBUG || defined UNIV_DEBUG */
using Col_offsets_t = ulint;

#endif /* univ_i */
