set(module bb)
set(MODULE BB)
configure_file(${PROJECT_SOURCE_DIR}/mem/mem.h.in mem_bb.h @ONLY)
add_library(bb
  averager.c
  bb_asprintf.c
  bb_daemon.c
  bb_getopt_long.c
  bb_oscompat.c
  bbhrtime.c
  cheapstub.c
  comdb2_pthread_create.c
  comdb2file.c
  ctrace.c
  debug_switches.c
  flibc.c
  fsnapf.c
  int_overflow.c
  intern_strings.c
  list.c
  lockassert.c
  logmsg.c
  misc.c
  nodemap.c
  object_pool.c
  plhash.c
  pool.c
  pooltest.c
  portmuxusr.c
  queue.c
  queuetest.c
  roll_file.c
  rtcpu.c
  safestrerror.c
  sbuf2.c
  segstring.c
  sltpck.c
  ssl_support.c
  str0.c
  strbuf.c
  switches.c
  tcputil.c
  thdpool.c
  thread_malloc.c
  thread_util.c
  timers.c
  utilmisc.c
  walkback.c
  xstring.c
)
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${PROJECT_SOURCE_DIR}/bbinc
  ${PROJECT_SOURCE_DIR}/cdb2api
  ${PROJECT_SOURCE_DIR}/dlmalloc
  ${PROJECT_SOURCE_DIR}/mem
  ${PROJECT_BINARY_DIR}/mem
  ${PROJECT_SOURCE_DIR}/sockpool
  ${OPENSSL_INCLUDE_DIR}
  ${UNWIND_INCLUDE_DIR}
)
add_dependencies(bb mem)
set_source_files_properties(walkback.c PROPERTIES COMPILE_FLAGS -DUSE_UNWIND)
