/* SPDX-License-Identifier: Apache-2.0 */
/* Guest-only PT_INTERP companion; see module_exec_runtime_probe.c. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
int main(int argc,char **argv)
{
    struct utsname identity;
    if(uname(&identity) || strcmp(identity.sysname,"ReliefNT"))return 77;
    char name[4096]={0};ssize_t size=readlink("/proc/self/exe",name,sizeof(name)-1);
    for(int i=1;i+1<argc;++i)if(!strcmp(argv[i],"--exec-path-child")) {
        if(size<0 || strcmp(name,argv[i+1])) {
            fprintf(stderr,"dynamic exe mismatch: %s expected %s\n",name,argv[i+1]);return 1;
        }
        if(i+3<argc) {
            int notify=atoi(argv[i+2]),ack=atoi(argv[i+3]);char byte='R';
            if(write(notify,&byte,1)!=1 || read(ack,&byte,1)!=1 || byte!='A')return 1;
            close(notify);close(ack);
            /* libc/loader text remains valid after the interpreter is renamed. */
            if(printf("MODULE_RUNTIME_INTERPRETER retained provider after rename\n")<0)return 1;
        }
        return 42;
    }
    return 1;
}
