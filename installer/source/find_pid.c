// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sdk_shim.h"
#include "kern_rw_fast.h"

//https://github.com/ps5-payload-dev/klogsrv/blob/c1b64bd3d3b069a09b1a173a82858cba97a5656f/main.c#L269
int find_proc_pid_by_name(const char *name)
{
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }

  if(!(buf=malloc(buf_size))) {
    perror("malloc");
    return -1;
  }

  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    free(buf);
    return -1;
  }

  for(uint8_t *ptr=buf; ptr<(buf+buf_size);) {
    int ki_structsize = *(int*)ptr;
    pid_t ki_pid = *(pid_t*)&ptr[72];
    char *ki_tdname = (char*)&ptr[447];

    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && ki_pid != mypid) {
      pid = ki_pid;
    }
  }

  free(buf);

  return pid;
}
