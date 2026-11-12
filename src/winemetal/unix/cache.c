#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "sqlite3.h"
#define WINEMETAL_API
#include "../winemetal_thunks.h"

@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end

@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (void)set:(NSData *)key value:(dispatch_data_t)value;
@end

@interface CacheReader () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
  uint64_t _hits, _misses;
  bool _stats;
}
@end

/* MADEIRA: DXMT_IOS_CACHE_DIR. Darwin's per-user cache confstr
 * (_CS_DARWIN_USER_CACHE_DIR) is not available inside the iOS sandbox, so a
 * relative cache path resolves to nothing and neither the Metal nor the DXMT
 * shader cache persists. With the switch on, a relative path resolves under
 * the app's NSCachesDirectory instead and a directory that cannot be created
 * fails cleanly.
 *
 * Decided per caller, like winemetal_unix.c's madeira_switch_for_caller: on by
 * default for a caller in a 32-bit (WoW64) pseudo-process (ios_wow_base() is
 * its guest-window base), off for a 64-bit caller, which keeps the upstream
 * lookup below. "0" disables it for every caller, any other non-empty value
 * enables it for every caller. */
#if TARGET_OS_IPHONE
extern unsigned long ios_wow_base(void);

static bool
use_ios_cache_dir(void) {
  const char *e = getenv("DXMT_IOS_CACHE_DIR");
  if (e && *e)
    return strcmp(e, "0") != 0;
  return ios_wow_base() != 0;
}

static NSString *
resolve_ios_cache_dir(NSString *path, bool path_is_file) {
  if (!path.length)
    return nil;
  if (![path hasPrefix:@"/"]) {
    NSString *base = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      NSLog(@"[shader-cache] ml1160 app cache directory %@", base ? @"available" : @"unavailable");
    });
    if (!base)
      return nil;
    path = [base stringByAppendingPathComponent:path];
  }
  if (![[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil])
    return nil;
  return path;
}
#endif

static inline NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
#if TARGET_OS_IPHONE
  if (use_ios_cache_dir())
    return resolve_ios_cache_dir(path, path_is_file);
#endif
  if (![path hasPrefix:@"/"]) {
    char buf[PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));

    if (!len) {
      return nil;
    }

    NSString *base = [NSString stringWithUTF8String:buf];
    path = [base stringByAppendingPathComponent:path];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return path;
}

@implementation CacheReader

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    /* Opt-in diagnostics: DXMT_CACHE_STATS=1 logs hit/miss counts. */
    const char *stats = getenv("DXMT_CACHE_STATS");
    _stats = stats && *stats && strcmp(stats, "0");
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }
    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    if (sqlite3_open_v2([dbPath fileSystemRepresentation], &_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to open DB: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }

    NSString *sqlGet = [NSString stringWithFormat:@"SELECT value FROM %@ WHERE key = ?;", tableName];
    if (sqlite3_prepare_v2(_db, sqlGet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to prepare SELECT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (dispatch_data_t)get:(NSData *)key {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  dispatch_data_t result = nil;
  if (sqlite3_step(_stmt) == SQLITE_ROW) {
    const void *bytes = sqlite3_column_blob(_stmt, 0);
    int len = sqlite3_column_bytes(_stmt, 0);
    result = dispatch_data_create(bytes, len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  }
  sqlite3_reset(_stmt);
  if (_stats) {
    if (result) ++_hits; else ++_misses;
    uint64_t count = _hits + _misses;
    if (count == 1 || count == 64 || count == 256 || !(count % 1024))
      NSLog(@"[shader-cache] ml1160 hits=%llu misses=%llu", _hits, _misses);
  }
  return result;
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

@interface CacheWriter () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

@implementation CacheWriter

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheWriter] Failed to resolve cache path");
      return nil;
    }

    NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
    int fd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      NSLog(@"[CacheWriter] Failed to open file for locking %@", lockPath);
      return nil;
    }
    flock(fd, LOCK_EX);

    if (sqlite3_open_v2(
            [dbPath fileSystemRepresentation], &_db, //
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL
        ) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to open DB: %s", sqlite3_errmsg(_db));
      flock(fd, LOCK_UN);
      close(fd);
      return nil;
    }

    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    NSString *sqlCreate = [NSString stringWithFormat:@"CREATE TABLE IF NOT EXISTS %@ ("
                                                      "key BLOB PRIMARY KEY, "
                                                      "value BLOB NOT NULL);",
                                                     tableName];

    char *errMsg = NULL;
    if (sqlite3_exec(_db, sqlCreate.UTF8String, NULL, NULL, &errMsg) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to create table: %s", errMsg);
      sqlite3_free(errMsg);
    }

    flock(fd, LOCK_UN);
    close(fd);

    NSString *sqlSet = [NSString stringWithFormat:@"INSERT OR REPLACE INTO %@ (key, value) VALUES (?, ?);", tableName];
    if (sqlite3_prepare_v2(_db, sqlSet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to prepare INSERT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (void)set:(NSData *)key value:(dispatch_data_t)value {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t flat = dispatch_data_create_map(value, &bytes, &length);
  sqlite3_bind_blob64(_stmt, 2, bytes, length, SQLITE_STATIC);

  if (sqlite3_step(_stmt) != SQLITE_DONE) {
    NSLog(@"[CacheWriter] Failed to insert: %s", sqlite3_errmsg(_db));
  }

  dispatch_release(flat);
  sqlite3_reset(_stmt);
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

int
_CacheReader_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheReader alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheReader_get(void *obj) {
  struct unixcall_cache_get *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheReader *reader = (CacheReader *)params->cache;
  params->ret_data = (obj_handle_t)[reader get:key];
  [key release];
  return 0;
}

int
_CacheWriter_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheWriter alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheWriter_set(void *obj) {
  struct unixcall_cache_set *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheWriter *writer = (CacheWriter *)params->cache;
  [writer set:key value:(dispatch_data_t)params->value_data];
  [key release];
  return 0;
}

#ifndef DXMT_NO_PRIVATE_API

extern void MTLSetShaderCachePath(NSString* path);
extern NSString* MTLGetShaderCachePath();

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
#if TARGET_OS_IPHONE
  /* With DXMT_IOS_CACHE_DIR on, an unresolvable path leaves Metal's cache
   * path alone instead of setting it to nil. */
  if (!resolved_path && use_ios_cache_dir()) {
    params->ret_success = 0;
    [path release];
    return 0;
  }
#endif
  MTLSetShaderCachePath(resolved_path);
  params->ret_success = [MTLGetShaderCachePath() isEqualToString:resolved_path];
  [path release];
  return 0;
}

#else

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  params->ret_success = 0;
  return 0;
}

#endif
