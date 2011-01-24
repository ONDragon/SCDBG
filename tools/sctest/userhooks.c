/********************************************************************************
 *                               libemu
 *
 *                    - x86 shellcode emulation -
 *
 *
 * Copyright (C) 2007  Paul Baecher & Markus Koetter
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 * 
 *             contact nepenthesdev@users.sourceforge.net  
 *
 *******************************************************************************/


#include "../config.h"

#define HAVE_GETOPT_H
#ifdef HAVE_GETOPT_H
# include <getopt.h>
#endif

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif


#include <stdint.h>

#define HAVE_UNISTD
#ifdef HAVE_UNISTD
# include <unistd.h>
#endif
#include <stdio.h>

#include <stdarg.h>


#include <errno.h>
#include <sys/select.h>

#include <sys/wait.h>

#ifdef HAVE_LIBCARGOS
#include <cargos-lib.h>
#endif


#include <sys/types.h>
#include <sys/socket.h>  

#include "emu/emu.h"
#include "emu/emu_memory.h"
#include "emu/emu_cpu.h"
#include "emu/emu_log.h"
#include "emu/emu_cpu_data.h"
#include "emu/emu_cpu_stack.h"
#include "emu/environment/emu_profile.h"
#include "emu/environment/emu_env.h"
#include "emu/environment/win32/emu_env_w32.h"
#include "emu/environment/win32/emu_env_w32_dll.h"
#include "emu/environment/win32/emu_env_w32_dll_export.h"
#include "emu/environment/win32/env_w32_dll_export_kernel32_hooks.h"
#include "emu/environment/linux/emu_env_linux.h"
#include "emu/emu_getpc.h"
#include "emu/emu_graph.h"
#include "emu/emu_string.h"
#include "emu/emu_hashtable.h"

#include "emu/emu_shellcode.h"


#include "userhooks.h"
#include "options.h"
#include "nanny.h"

#include <stdint.h>
#include <stdarg.h>


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

extern int CODE_OFFSET;
extern void hexdump(unsigned char*, int);
extern struct emu_memory *mem;

//by the time our user call is called, the args have already been popped off the stack.
//in r/t that just means that esp has been adjusted and cleaned up for function to 
//return, since there hasnt been any memory writes, we can still grab the return address
//off the stack if we know the arg sizes and calculate it with teh adjustment.
//little bit more work, but safe and doesnt require any otherwise sweeping changes
//to the dll - dzzie

uint32_t get_ret(struct emu_env *env, int arg_adjust){

	struct emu_memory *m = emu_memory_get(env->emu);
	uint32_t reg_esp = emu_cpu_reg32_get( emu_cpu_get(env->emu), esp);
	uint32_t ret_val = 0;
	
	emu_memory_read_dword( m, reg_esp+arg_adjust, &ret_val);
	
	if(opts.adjust_offsets){
		if( (ret_val > CODE_OFFSET) &&  (ret_val <= (CODE_OFFSET + opts.size)) ){
			return ret_val - CODE_OFFSET; //adjusted to file offset of input file
		}else{
			return ret_val; //return the raw value from stack
		}
	}else{
		return ret_val; //return the raw value from stack
	}

}

char* get_client_ip(struct sockaddr *clientInformation)
{	
	if (clientInformation->sa_family == AF_INET) {
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)clientInformation;
		return inet_ntoa(ipv4->sin_addr);
	}
	return 0;
}

unsigned int get_client_port(struct sockaddr *clientInformation)
{
	unsigned int portNumber;
	if (clientInformation->sa_family == AF_INET) {
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)clientInformation;
		portNumber = ntohs(ipv4->sin_port);
		return portNumber;
	}
	return 0;
}

void append(struct emu_string *to, const char *dir, char *data, int size)
{
	char *saveptr = data;

	struct emu_string *sanestr = emu_string_new();


	int i;
	for (i=0;i<size;i++)
	{
		if (data[i] == '\r')
		{

		}else
		if ( isprint((int)data[i]))// || isblank(data[i]))
		{
			emu_string_append_format(sanestr, "%c", data[i]);
		}
		else
		if (data[i] == '\n')
		{
			emu_string_append_char(sanestr, "\n");
		}
		else
		if (data[i] == '\t')
		{
			emu_string_append_char(sanestr, "\t");
		} 
		else
		{
			emu_string_append_format(sanestr, "\\x%02x", (unsigned char)data[i]);
		}
	}

	saveptr = NULL;


	char *tok;
	tok  = strtok_r(sanestr->data, "\n", &saveptr);
//	printf("line %s:%s\n",dir, tok);
	if (tok != NULL)
	{
		emu_string_append_format(to, "%s %s\n", dir, tok); 
		while ( (tok = strtok_r(NULL,"\n",&saveptr)) != NULL )
		{
			emu_string_append_format(to, "%s %s\n", dir, tok);
//		printf("line %s:%s\n",dir, tok);
		}

	}
	emu_string_free(sanestr);
}



// ------------------------ HOOKS BELOW HERE -------------------------------

uint32_t user_hook_ExitProcess(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -8);

/*
VOID WINAPI ExitProcess(
  UINT uExitCode
);
*/

	va_list vl;
	va_start(vl, hook);
	int exitcode = va_arg(vl,  int);
	va_end(vl);

	printf("%x\t%s(%i)\n", retaddr, hook->hook.win->fnname, exitcode);


	opts.steps = 0;
	return 0;
}


uint32_t user_hook_ExitThread(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -8);

/*
VOID ExitThread(
  DWORD dwExitCode
);
*/

	va_list vl;
	va_start(vl, hook);
	int exitcode = va_arg(vl,  int);
	va_end(vl);

	printf("%x\t%s(%i)\n",retaddr, hook->hook.win->fnname, exitcode);

	opts.steps = 0;
	return 0;

}

uint32_t user_hook_CreateProcess(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -1*((8*4)+4) );

	va_list vl;
	va_start(vl, hook);

	/* char *pszImageName				  = */ (void)va_arg(vl, char *);
	char *pszCmdLine                      = va_arg(vl, char *);               

	//STARTUPINFO *psiStartInfo             = va_arg(vl, STARTUPINFO *);
	//PROCESS_INFORMATION *pProcInfo        = va_arg(vl, PROCESS_INFORMATION *); 

	va_end(vl);

	printf("%x\tCreateProcess(%s)\n",retaddr, pszCmdLine );

	if(opts.interactive_hooks == 0) return 1;

	if ( pszCmdLine != NULL && strncasecmp(pszCmdLine, "cmd", 3) == 0 )
	{
		//todo possibly do stuff here to capture command line sent to cmd...
	}

	return 1;
}

uint32_t user_hook_WaitForSingleObject(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-12);


	/*
	DWORD WINAPI WaitForSingleObject(
	  HANDLE hHandle,
	  DWORD dwMilliseconds
	);
	*/

	va_list vl;
	va_start(vl, hook);

	int32_t hHandle = va_arg(vl, int32_t);
	/*int32_t dwMilliseconds = */ (void)va_arg(vl, int32_t);
	va_end(vl);

	printf("%x\tWaitForSingleObject(h=%x)\n",retaddr, (int)hHandle);

	return 0;
}


uint32_t user_hook_exit(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-8);

	va_list vl;
	va_start(vl, hook);
	int code = va_arg(vl,  int);
	va_end(vl);

	printf("%x\texit(%x)\n",retaddr,code );

	opts.steps = 0;
	return 0;
}

uint32_t user_hook_accept(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-16);

	va_list vl;
	va_start(vl, hook);

	int s 					= va_arg(vl,  int);
	/*struct sockaddr* addr 	= */(void)va_arg(vl,  struct sockaddr *);
	/*socklen_t* addrlen 		= */(void)va_arg(vl,  socklen_t *);
	va_end(vl);

	struct sockaddr sa;
	socklen_t st = sizeof(struct sockaddr);

	printf("%x\taccept(h=%x)\n",retaddr, (int)s);

    return accept(s, &sa, &st);
}

uint32_t user_hook_bind(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-16);

	va_list vl;
	va_start(vl, hook);

	int s 					= va_arg(vl,  int);
	struct sockaddr* addr 	= va_arg(vl,  struct sockaddr *);
	socklen_t addrlen = va_arg(vl,  socklen_t );

	/*
	if (opts.override.bind.host != NULL )
	{
		struct sockaddr_in *si = (struct sockaddr_in *)addr;
		si->sin_addr.s_addr = inet_addr(opts.override.bind.host);
	}

	if (opts.override.bind.port > 0)
	{
		struct sockaddr_in *si = (struct sockaddr_in *)addr;;
		si->sin_port = htons(opts.override.bind.port);
	}
*/
	va_end(vl);

	printf("%x\tbind(port: %d )\n",retaddr, get_client_port(addr) );

    return bind(s, addr, addrlen);
}

uint32_t user_hook_closesocket(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-8);

	va_list vl;
	va_start(vl, hook);
	int s 					= va_arg(vl,  int);
	va_end(vl);

	printf("%x\tclosesocket(h=%x)\n",retaddr,(int)s );

    return close(s);
}

uint32_t user_hook_connect(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-16);

	va_list vl;
	va_start(vl, hook);

	int s 					= va_arg(vl,  int);
	struct sockaddr* addr 	= va_arg(vl,  struct sockaddr *);


	if (opts.override.connect.host != NULL )
	{
		struct sockaddr_in *si = (struct sockaddr_in *)addr;
		si->sin_addr.s_addr = inet_addr(opts.override.connect.host);
	}

	if (opts.override.connect.port > 0)
	{
		struct sockaddr_in *si = (struct sockaddr_in *)addr;;
		si->sin_port = htons(opts.override.connect.port);
	}

	socklen_t addrlen = va_arg(vl,  socklen_t);

	if (addrlen != sizeof(struct sockaddr))
	{
		addrlen = sizeof(struct sockaddr);
	}

	va_end(vl);

	printf("%x\tconnect(h=%x, host: %s , port: %d )\n",retaddr,s, get_client_ip(addr), get_client_port(addr) );

	if( opts.interactive_hooks == 0 ) return 0x4711;

	return connect(s, addr, addrlen);
	
}

uint32_t user_hook_fclose(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);
//int fclose(FILE *fp);

	uint32_t retaddr = get_ret(env,-8);

	va_list vl;
	va_start(vl, hook);
	FILE *f = va_arg(vl, FILE *);
	va_end(vl);

	printf("%x\tfclose(h=%x)\n",retaddr, (int)f);

	if( opts.interactive_hooks == 0 )  return 0x4711;

	struct nanny_file *nf = nanny_get_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)f);

	if (nf != NULL)
	{
		FILE *ef = nf->real_file;
		nanny_del_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)f);
    	return fclose(ef);
	}
	else 
		return 0;
	

}


uint32_t user_hook_fopen(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	char *localfile;
	uint32_t retaddr = get_ret(env,-12);

	va_list vl;
	va_start(vl, hook);
	char *filename			= va_arg(vl,  char *);
	/*char *mode 				= */(void)va_arg(vl,  char *);
	va_end(vl);

	if( opts.interactive_hooks == 0){
		printf("%x\tfopen(%s) = %x\n", retaddr, filename, 0x4711);
		return 0x4711;
	}

	if ( asprintf(&localfile, "/tmp/%s-XXXXXX",filename) == -1) return -1;

	int fd = mkstemp(localfile);
	close(fd);

	FILE *f = fopen(localfile,"w");
	
	printf("%x\tfopen(%s) = %x\n", retaddr, filename, (int)f);

	uint32_t file;
	nanny_add_file(hook->hook.win->userdata, localfile, &file, f);

	return file;
}

uint32_t user_hook_fwrite(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-20);

/*       size_t fwrite(const void *ptr, size_t size, size_t nmemb,
                     FILE *stream);
*/
	va_list vl;
	va_start(vl, hook);
	void *data = va_arg(vl, void *);
	size_t size = va_arg(vl, size_t);
	size_t nmemb = va_arg(vl, size_t);
	FILE *f = va_arg(vl, FILE *);
	va_end(vl);
	
	printf("%x\tfwrite(h=%x, sz=%x, buf=%x)\n", retaddr, (int)f, size, (int)data);
	
	if(opts.interactive_hooks == 0 ) return size*nmemb;

	struct nanny_file *nf = nanny_get_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)f);

	if (nf != NULL)
		return fwrite(data, size, nmemb, nf->real_file);
	else 
		return size*nmemb;

}



uint32_t user_hook_listen(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-12);

	va_list vl;
	va_start(vl, hook);

	int s 					= va_arg(vl,  int);
	int backlog			 	= va_arg(vl,  int);
	va_end(vl);
	
	printf("%x\tlisten(h=%x)\n",retaddr,s);

	if(opts.interactive_hooks == 0 ) return 0; //ok

    return listen(s, backlog);
}

uint32_t user_hook_recv(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-20);
	uint32_t real_buf = get_ret(env,-12); //grab the real buf addr off of stack..

	va_list vl;
	va_start(vl, hook);

	int s = va_arg(vl,  int);
	char* buf = va_arg(vl,  char *); //not org buffer, libemu malloced
	int len = va_arg(vl,  int);
	int flags = va_arg(vl,  int);
	va_end(vl);
	 
	printf("%x\trecv(h=%x, buf=%x, len=%x, fl=%x)\n", retaddr, s, real_buf, len, flags);
	
	if(opts.interactive_hooks == 0 ) return 0; //no data

	int ret_val=0;

	ret_val = recv(s, buf, len,  flags); //libemu malloced buf
	
	if(opts.show_hexdumps && ret_val > 0){
		printf("%d bytes received:\n", ret_val);
		hexdump(buf, ret_val);
	}

	return ret_val; //if we return > 0 dll will write it to mem at real emu addr for us..

}

uint32_t user_hook_send(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-16);

	va_list vl;
	va_start(vl, hook);

	int s = va_arg(vl,  int);
	char* buf = va_arg(vl,  char *);
	int len = va_arg(vl,  int);
	int flags = va_arg(vl,  int);
	va_end(vl);

	printf("%x\tsend(h=%x, buf=%x, len=%x)\n",retaddr, s, (int)buf, len);

	if(opts.interactive_hooks == 0 ) return len; //success

	return send(s, buf, len,  flags);
}


uint32_t user_hook_socket(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-16);

	va_list vl;
	va_start(vl, hook);
	/* int socket(int domain, int type, int protocol); */
	int domain = va_arg(vl,  int);
	int type = va_arg(vl,  int);
	int protocol = va_arg(vl, int);
	va_end(vl);

	printf("%x\tsocket(%i, %i, %i)\n",retaddr, domain, type, protocol);

	if(opts.interactive_hooks == 0 ) return 0x4711;

	return socket(domain, type, protocol);
}

uint32_t user_hook_WSASocket(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -1*((6*4)+4) );

	va_list vl;
	va_start(vl, hook);
	/* int socket(int domain, int type, int protocol); */
	int domain = va_arg(vl,  int);
	int type = va_arg(vl,  int);
	int protocol = va_arg(vl, int);
	(void)va_arg(vl, int);
	(void)va_arg(vl, int);
	(void)va_arg(vl, int);

	va_end(vl);

	printf("%x\tWSASocket(%i, %i, %i)\n",retaddr, domain, type, protocol);

	if(opts.interactive_hooks == 0 ) return 0x4711;

	return socket(domain, type, protocol);
}


uint32_t user_hook_CreateFile(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -1*((7*4)+4));

/*
HANDLE CreateFile(
  LPCTSTR lpFileName,
  DWORD dwDesiredAccess,
  DWORD dwShareMode,
  LPSECURITY_ATTRIBUTES lpSecurityAttributes,
  DWORD dwCreationDisposition,
  DWORD dwFlagsAndAttributes,
  HANDLE hTemplateFile
);
*/

	va_list vl;
	va_start(vl, hook);
	char *lpFileName			= va_arg(vl, char *);
	/*int dwDesiredAccess		=*/(void)va_arg(vl, int);
	/*int dwShareMode			=*/(void)va_arg(vl, int);
	/*int lpSecurityAttributes	=*/(void)va_arg(vl, int);
	/*int dwCreationDisposition	=*/(void)va_arg(vl, int);
	/*int dwFlagsAndAttributes	=*/(void)va_arg(vl, int);
	/*int hTemplateFile			=*/(void)va_arg(vl, int);
	va_end(vl);

	char *localfile;
	printf("%x\tCreateFile(%s)\n",retaddr,lpFileName);

	if(opts.interactive_hooks == 0 ) return 0x4444;

	if ( asprintf(&localfile, "/tmp/%s-XXXXXX",lpFileName) == -1) return -1; //exit(-1);

	int fd = mkstemp(localfile);
	close(fd);

	FILE *f = fopen(localfile,"w");

	uint32_t handle;
	nanny_add_file(hook->hook.win->userdata, localfile, &handle, f);

	return (uint32_t)handle;
}

uint32_t user_hook_WriteFile(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -1*((5*4)+4));

/*
BOOL WriteFile(
  HANDLE hFile,
  LPCVOID lpBuffer,
  DWORD nNumberOfBytesToWrite,
  LPDWORD lpNumberOfBytesWritten,
  LPOVERLAPPED lpOverlapped
);
*/

	int written = -1;
	va_list vl;
	va_start(vl, hook);
	FILE *hFile 					= va_arg(vl, FILE *);
	void *lpBuffer 					= va_arg(vl, void *);
	int   nNumberOfBytesToWrite 	= va_arg(vl, int);
	/* int *lpNumberOfBytesWritten  =*/(void)va_arg(vl, int*);
	/* int *lpOverlapped 		    =*/(void)va_arg(vl, int*);
	va_end(vl);

	printf("%x\tWriteFile(h=%x, buf=%x)\n",retaddr,(int)hFile,(int)lpBuffer);

	if(opts.interactive_hooks == 0 ) return 1; //success

	struct nanny_file *nf = nanny_get_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)hFile);

	if (nf != NULL){
		written = fwrite(lpBuffer, nNumberOfBytesToWrite, 1, nf->real_file);
	}else{
		//printf("shellcode tried to write data to not existing handle\n");
	}

	return 1;

}


uint32_t user_hook_CloseHandle(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-8);

/*
BOOL CloseHandle(
  HANDLE hObject
);
*/

	va_list vl;
	va_start(vl, hook);
	FILE *hObject = va_arg(vl, FILE *);
	va_end(vl);

	printf("%x\tCloseHandle(%x)\n",retaddr,(int)hObject);

	if(opts.interactive_hooks == 0 ) return 1; //success

	struct nanny_file *nf = nanny_get_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)hObject);

	if (nf != NULL)
	{
		FILE *f = nf->real_file;
		nanny_del_file(hook->hook.win->userdata, (uint32_t)(uintptr_t)hObject);
		fclose(f);
	}
	else 
	{
		//printf("shellcode tried to close not existing handle (maybe closed it already?)\n");
	}


	return 1;
}



uint32_t user_hook_URLDownloadToFile(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-24);

	va_list vl;
	va_start(vl, hook);

	/*void * pCaller    = */(void)va_arg(vl, void *);
	char * szURL      = va_arg(vl, char *);
	char * szFileName = va_arg(vl, char *);
	/*int    dwReserved = */(void)va_arg(vl, int   );
	/*void * lpfnCB     = */(void)va_arg(vl, void *);

	va_end(vl);

	printf("%x\tURLDownloadToFile(%s, %s)\n",retaddr, szURL, szFileName);

	return 0;
}




// ---------------- added 1-20-11 dzzie (not all have been tested live! ) ---------------

uint32_t user_hook_GetProcAddress(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-12);

/*
h GetProcAddress(hModule, proc)
);
*/

	va_list vl;
	va_start(vl, hook);
	/*int hMod  = */ va_arg(vl,  int);
	char* api = va_arg(vl, char *); 
	va_end(vl);

	printf("%x\tGetProcAddress(%s)\n",retaddr, api);

	return 0;

}


uint32_t user_hook_GetSystemDirectoryA(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-12);

/*
UINT GetSystemDirectory(
  LPTSTR lpBuffer,
  UINT uSize
);
*/

	printf("%x\tGetSystemDirectoryA(c:\\windows\\system32\\)\n",retaddr);

	return 0;

}


uint32_t user_hook_GetTickCount(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-4);

	printf("%x\tGetTickCount()\n",retaddr);

	return 0;

}


uint32_t user_hook_LoadLibraryA(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-8);

/*
lpLibName
);
*/

	va_list vl;
	va_start(vl, hook);
	char* lib = va_arg(vl, char *); 
	va_end(vl);

	printf("%x\tLoadLibrary(%s)\n",retaddr,lib);

	return 0;

}


uint32_t user_hook__lcreat(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-12);

/*
LONG _lcreat(
  LPCSTR lpszFileName,
  int fnAttribute
);
*/
	va_list vl;
	va_start(vl, hook);
	char* fname = va_arg(vl, char *); 
	/*int fnAttribute  = */ va_arg(vl,  int);
	va_end(vl);

	printf("%x\t_lcreate(%s)\n",retaddr,fname);

	return 0;

}


uint32_t user_hook__lwrite(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-16);

/*
LONG _lwrite(
  HFile hFile,
  LPCSTR lpBuffer,
  UINT cbWrite
);
*/

	va_list vl;
	va_start(vl, hook);
	int hFile    = va_arg(vl,  int);
	int lpBuffer = va_arg(vl,  int);
	/*int cbWrite  =*/ va_arg(vl,  int);
	va_end(vl);

	printf("%x\t_lwrite(h=%x, buf=%x)\n",retaddr, hFile, lpBuffer);

	return 0;

}


uint32_t user_hook__lclose(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-8);

/*
prototype
);
*/

	va_list vl;
	va_start(vl, hook);
	int hFile  =  va_arg(vl,  int); 
	va_end(vl);

	printf("%x\t_lclose(h=%x)\n",retaddr,hFile);

	return 0;

}


uint32_t user_hook_malloc(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-8);

/*
void *malloc( 
   size_t size 
);
*/

	va_list vl;
	va_start(vl, hook);
	int sz  =  va_arg(vl,  int);
	va_end(vl);

	printf("%x\tmalloc(%x)\n",retaddr,sz);

	return 0;

}


uint32_t user_hook_memset(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-16);

/*
void *memset(
   void* dest, 
   int c, 
   size_t count 
);
*/

	va_list vl;
	va_start(vl, hook);
	int dest  =   va_arg(vl,  int);
	int c  =   va_arg(vl,  int);
	int sz  =   va_arg(vl,  int);
	va_end(vl);

	printf("%x\tmemset(buf=%x, c=%x, sz=%x)\n",retaddr,dest,c,sz);

	return 0;

}


uint32_t user_hook_SetUnhandledExceptionFilter(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-8);

/*
lpFilter
);
*/

	va_list vl;
	va_start(vl, hook);
	int lpfilter  =  va_arg(vl,  int);
	va_end(vl);

	printf("%x\tSetUnhandledExceptionFilter(%x)\n",retaddr,lpfilter);

	return 0;

}


uint32_t user_hook_WinExec(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr =  get_ret(env,-12);

/*
UINT WINAPI WinExec(
  LPCSTR lpCmdLine,
  UINT uCmdShow
);
*/

	va_list vl;
	va_start(vl, hook);
	char* cmd = va_arg(vl, char *); 
	/*int uCmdShow  = */ va_arg(vl,  int);
	va_end(vl);

	printf("%x\tWinExec(%s)\n",retaddr,cmd);

	return 0;

}


//---------------------------------- added with newest dll ---------------

uint32_t user_hook_DeleteFileA(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-8);

	/*
	BOOL DeleteFile(
	  LPCTSTR lpFileName
	);
	*/

	va_list vl;
	va_start(vl, hook);

	char* fpath = va_arg(vl, char*);
	va_end(vl);

	printf("%x\tDeleteFileA(%s)\n",retaddr, fpath );

	return 0;
}

uint32_t user_hook_GetVersion(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-4);

/*
DWORD WINAPI GetVersion(void);
*/
	printf("%x\tGetVersion()\n",retaddr);

	return 0;
}

uint32_t user_hook_GetTempPath(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-12);

/*
DWORD WINAPI GetTempPath(
  __in   DWORD nBufferLength,
  __out  LPTSTR lpBuffer
);
*/
	printf("%x\tGetTempPath()\n",retaddr);

	return 0;
}

uint32_t user_hook_Sleep(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env,-8);

/*VOID WINAPI Sleep(
  __in  DWORD dwMilliseconds
);
*/
	va_list vl;
	va_start(vl, hook);
	int32_t dwMilliseconds = va_arg(vl, int32_t);
	va_end(vl);

	printf("%x\tSleep(0x%x)\n",retaddr, dwMilliseconds);

	return 0;
}

uint32_t user_hook_VirtualProtect(struct emu_env *env, struct emu_env_hook *hook, ...)
{
	//printf("Hook me Captain Cook!\n");
	//printf("%s:%i %s\n",__FILE__,__LINE__,__FUNCTION__);

	uint32_t retaddr = get_ret(env, -24);

/*
 * BOOL VirtualProtect( 
 *	LPVOID lpAddress, 
 *	DWORD  dwSize, 
 *      DWORD  flNewProtect, 
 *      PDWORD lpflOldProtect 
 *); 
 */
	va_list vl;
	va_start(vl, hook);
	int32_t addr = va_arg(vl, int32_t);
	int32_t size = va_arg(vl, int32_t);
	va_end(vl);

	printf("%x\tVirtualProtect(adr=%x, sz=%x)\n",retaddr, addr, size );

	return 0;
}


/* this is confirmed as working :Dzzie 
   this one is just an example i used for testing. 
   will add real ones as i find shellcode that need them.

int32_t	new_user_hook_closesocket(struct emu_env *env, struct emu_env_hook *hook)
{

	struct emu_cpu *c = emu_cpu_get(env->emu);

	uint32_t eip_save;

	POP_DWORD(c, &eip_save);
 
	uint32_t s;
	POP_DWORD(c, &s);

	printf("support for new user hooks confirmed! my closesocket called\n");
	
	emu_cpu_reg32_set(c, eax, 0);
	emu_cpu_eip_set(c, eip_save);
	return 0;
}
*/

