/*
   UnrealIRCd internal webserver
   Copyright (c) 2001, The UnrealIRCd Team
   All rights reserved.

   Redistribution and use in source and binary forms, with or without modification, are permitted
   provided that the following conditions are met:
   
     * Redistributions of source code must retain the above copyright notice, this list of conditions
       and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright notice, this list of conditions
       and the following disclaimer in the documentation and/or other materials provided with the
       distribution.
     * Neither the name of the The UnrealIRCd Team nor the names of its contributors may be used
       to endorse or promote products derived from this software without specific prior written permission.
     * The source code may not be redistributed for a fee or in closed source
       programs, without expressed oral consent by the UnrealIRCd Team, however
       for operating systems where binary distribution is required, if URL
       is passed with the package to get the full source
     * No warranty is given unless stated so by the The UnrealIRCd Team

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
   BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR  
   BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "config.h"
#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "msg.h"
#include "channel.h"
#include "inet.h"
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#endif
#include <fcntl.h>
#include "h.h"
#ifdef _WIN32
#include "version.h"
#endif
#include "threads.h"
#include "modules/web/httpd.h"

SOCKET	httpdfd = -1;

int	h_u_stats(HTTPd_Request *r);
int	h_u_vfs(HTTPd_Request *r);
int	h_u_phtml(HTTPd_Request *r);

void	httpd_setup_acceptthread(void);
void	httpd_acceptthread(void *p);
void	httpd_socketthread(void *preq);

void	sockprintf(HTTPd_Request *r, char *format, ...);
int	httpd_parse(HTTPd_Request *request);
void	httpd_badrequest(HTTPd_Request *request, char *reason);
void	httpd_parse_final(HTTPd_Request *request);
void 	httpd_404_header(HTTPd_Request *request, char *path);
Module *Mod_Handle = NULL;
static Hook *HttpdStats = NULL, *HttpdVfs = NULL, *HttpdPhtml = NULL;


#ifndef DYNAMIC_LINKING
ModuleHeader httpd_Header
#else
#define httpd_Header Mod_Header
ModuleHeader Mod_Header
#endif
  = {
	"httpd",	/* Name of module */
	"$Id$", /* Version */
	"httpd", /* Short description of module */
	"3.2-b5",
	NULL 
    };


#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Init(int module_load)
#else
int    httpd_Init(int module_load)
#endif
{
	HttpdStats = HookAddEx(Mod_Handle, HOOKTYPE_HTTPD_URL, h_u_stats);
	HttpdVfs = HookAddEx(Mod_Handle, HOOKTYPE_HTTPD_URL, h_u_vfs);
	HttpdPhtml = HookAddEx(Mod_Handle, HOOKTYPE_HTTPD_URL, h_u_phtml);
	return MOD_SUCCESS;
}

/* Is first run when server is 100% ready */
#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Load(int module_load)
#else
int    httpd_Load(int module_load)
#endif
{
	/* We set up the http socket */
	struct SOCKADDR_IN	sin;
	if ((httpdfd = socket(AFINET, SOCK_STREAM, 0)) == -1)
	{
		config_error("httpd: could not create socket: %s", strerror(ERRNO));
		httpdfd = -1;
		return MOD_FAILED;
	}
	set_non_blocking(httpdfd, NULL);
	set_sock_opts(httpdfd, NULL);		
#ifndef INET6
	sin.SIN_ADDR.S_ADDR = inet_addr("0.0.0.0");
#else
	inet_pton(AFINET, conf_listen->ip, (void *)&sin.SIN_ADDR);
#endif
	sin.SIN_PORT = htons(8091);
	sin.SIN_FAMILY = AFINET;
	
	if (bind(httpdfd, (struct SOCKADDR *)&sin, sizeof(sin)))
	{
		config_error("httpd: could not bind: %s",
			strerror(ERRNO));
		CLOSE_SOCK(httpdfd);
		httpdfd = -1;
		return MOD_FAILED;
	}
	listen(httpdfd, LISTEN_SIZE);
	httpd_setup_acceptthread();
	return MOD_SUCCESS;
}


/* Called when module is unloaded */
#ifdef DYNAMIC_LINKING
DLLFUNC int	Mod_Unload(int module_unload)
#else
int	httpd_Unload(int module_unload)
#endif
{
	HookDel(HttpdStats);
	HookDel(HttpdVfs);
	HookDel(HttpdPhtml);
	return MOD_SUCCESS;
}


void	httpd_setup_acceptthread(void)
{
	THREAD	thread;
	THREAD_ATTR thread_attr;
	
	IRCCreateThread(thread, thread_attr, httpd_acceptthread, NULL);
	return;
}

void	httpd_acceptthread(void	*p)
{
	fd_set		rfds;
	struct timeval  tv;
	int		retval;
	SOCKET		callerfd = -1; 
	THREAD		thread;
	THREAD_ATTR	thread_attr;
	HTTPd_Request	*req = NULL;
	
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	
	while (1)
	{
		FD_ZERO(&rfds);
		FD_SET(httpdfd, &rfds);
		if ((retval = select(httpdfd + 1, &rfds, NULL, NULL, NULL)))
		{
			callerfd = accept(httpdfd, NULL, NULL);
			if (callerfd >= 0)
			{
				req = (HTTPd_Request *) MyMallocEx(sizeof(HTTPd_Request));
				req->fd = callerfd;
				set_sock_opts(callerfd, NULL);
				IRCCreateThread(thread, thread_attr,
					 httpd_socketthread, (void *)req);
				req = NULL;
			}
			else
			{
				ircd_log(LOG_ERROR, "httpd: accept() error: %s",
					strerror(ERRNO));
			}
			
		}
	}	
}

void	httpd_socketthread(void *preq)
{
	HTTPd_Request *request = (HTTPd_Request *)preq;
        fd_set          rfds;
        struct timeval  tv;
        char		inbuf[1024];
	HTTPd_Header	*hp, *hp2;
	int		retval = 0, retval2 = 0;
	int		i;
        tv.tv_sec = 20;
	tv.tv_usec = 0;
        request->pos = 0;                            
	
	while (request->pos < 1024)
	{
	        tv.tv_sec = 3;
		tv.tv_usec = 0;
		FD_ZERO(&rfds);
		FD_SET(request->fd, &rfds);
		retval = select(request->fd + 1, &rfds, NULL, NULL, &tv);
		if (retval && FD_ISSET(request->fd, &rfds))
		{
			while ((retval2 = recv(request->fd, &inbuf[0], sizeof(inbuf), 0)) > 0)
			{
				for (i = 0; i <= retval2; i++)
				{
					request->inbuf[request->pos++] = inbuf[i];
					if (request->pos >= 1024)
						goto end;
					if (inbuf[i] == '\n')
					{
						request->inbuf[request->pos] = '\0';
						iCstrip(request->inbuf);
						if (httpd_parse(request) < 0)
						{
							/* We exit */
							goto end;
						}
						else
							request->pos = 0;
					}										
				}
			}
			goto end;
		}
		else
		{
			goto end;
		}
	}
	end:
	CLOSE_SOCK(request->fd);
	if (request->url)
		MyFree(request->url);
	
	hp = request->headers;
	while (hp)
	{
		if (hp->name)
			MyFree(hp->name);
		if (hp->value)
			MyFree(hp->value);
		hp2 = hp;
		hp = hp->next;
		MyFree(hp2);
	}
	hp = request->dataheaders;
	while (hp)
	{
		if (hp->name)
			MyFree(hp->name);
		if (hp->value)
			MyFree(hp->value);
		hp2 = hp;
		hp = hp->next;
		MyFree(hp2);
	}
	MyFree(request);
	IRCExitThread(NULL);
	return;
}


int	httpd_parse(HTTPd_Request *request)
{
	if (request->state == 0)
	{
		char	*cmd;

		cmd = strtok(request->inbuf, " ");
		if (!cmd)
		{
			httpd_badrequest(request, "Bad protocol start");
			return -1;
		}
		else
		{
			if (!strcmp(cmd, "GET") || !strcmp(cmd, "POST"))
			{
				char *url;
			
				url = strtok(NULL, " ");
				if (!url)
				{
					httpd_badrequest(request, "Missing parameter");
					return -1;
				}
				else
				{
					request->url = strdup(url);
					request->state = 1;
					request->method = !strcmp(cmd, "GET") ? 1 : 0;
					return 1;
				}
				
			}
			else
			{
				httpd_badrequest(request, cmd);
			}
			return -1;
		}
		return 1;
	}	
	else if (request->state == 1)
	{
		char	*headername;
		char	*headerdata;
		
		headername = strtok(request->inbuf, " ");
		if (!headername)
		{
			httpd_parse_final(request);
			if (request->state > 1)
			{
				return 1;
			}
			else
				return -1;
		}
		else
		{
			headerdata = strtok(NULL, "");
			if (!headerdata)
			{
				httpd_badrequest(request, "Malformed headers");
				return -1;
			}
			else
			{
				HTTPd_Header *header;
				header = (HTTPd_Header *) MyMalloc(sizeof(HTTPd_Header));
				header->name = strdup(headername);
				header->value = strdup(headerdata);
				header->next = request->headers;
				request->headers = header;
				return 1;
			}
		}
	} else if (request->state == 2)
	{
		if (parse_urlenc(request))
		{
			RunHookReturn(HOOKTYPE_HTTPD_URL, request, >0);
			httpd_404_header(request, request->url);
			return -1;
		}
		else
		{
			httpd_badrequest(request, "Bad encoding");
		}
		return -1;
	}
	return 1;
}

void	sockprintf(HTTPd_Request *r, char *format, ...)
{
	va_list		ap;

	va_start(ap, format);
	vsprintf(r->inbuf, format, ap);
	strcat(r->inbuf, "\r\n");
	va_end(ap);
	set_blocking(r->fd);
	send(r->fd, r->inbuf, strlen(r->inbuf), 0),
	set_non_blocking(r->fd, NULL);
}


void	httpd_badrequest(HTTPd_Request *request, char *reason)
{
	sockprintf(request, "HTTP/1.1 400 Bad Request");
	sockprintf(request, "Server: UnrealIRCd HTTPd");
	sockprintf(request, "Connection: close");
	sockprintf(request, "Content-Type: text/plain");
	sockprintf(request, "");
	sockprintf(request, "%s", reason);
}

/*	
 * copyright (c) 2000 Todor Prokopov
 * from the CGI library, under the GPL
*/
static int urldecode(char *s)
{
  char *p = s;

  while (*s != '\0')
  {
    if (*s == '%')
    {
      s++;
      if (!isxdigit(*s))
        return 0;
      *p = (isalpha(*s) ? (*s & 0xdf) - 'A' + 10 : *s - '0') << 4;
      s++;
      if (!isxdigit(*s))
        return 0;
      *p += isalpha(*s) ? (*s & 0xdf) - 'A' + 10 : *s - '0';
    }
    else if (*s == '+')
      *p = ' ';
    else
      *p = *s;
    s++;
    p++;
  }
  *p = '\0';
  return 1;
}

int parse_urlenc(HTTPd_Request *request)
{
  unsigned int i;
  unsigned int p = 0;
  int   param_count;
  int	content_length;
  char  **tempheaders = '\0';
  char  **tempvalues = '\0';
  HTTPd_Header *h;
  char *buf;

  content_length = request->content_length;
  buf = request->inbuf;

  if (content_length != 0)
  {
    param_count = 1;
    for (i = 0; i < content_length; i++)
      if (buf[i] == '&')
        param_count++;
    i = 0;
    p = 0;
    tempheaders = (char **)MyMallocEx(sizeof(char *) * param_count);
    tempvalues = (char **)MyMallocEx(sizeof(char *) * param_count);
    while (i < content_length)
    {
      tempheaders[p] = buf + i;
      while (i < content_length && buf[i] != '=' && buf[i] != '&')
        i++;
      if (i >= content_length || buf[i] != '=')
      {
	MyFree(tempheaders);
	MyFree(tempvalues);
        return 0;
      }
      buf[i] = '\0';
      i++;
      tempvalues[p] = buf + i;
      while (i < content_length && buf[i] != '=' && buf[i] != '&')
        i++;
      if (i < content_length)
      {
        if (buf[i] != '&')
        {
	  MyFree(tempheaders);
	  MyFree(tempvalues);
          return 0;
        }
        buf[i] = '\0';
        i++;
      }
      if (!urldecode(tempheaders[p]) || !urldecode(tempvalues[p]))
      {
	  MyFree(tempheaders);
	  MyFree(tempvalues);
        return 0;
      }
      p++;
    }
  }
  for (i = 0; i < p; i++)
  {
  	h = (HTTPd_Header *) MyMallocEx(sizeof(HTTPd_Header));
  	h->name = strdup(tempheaders[i]);
  	h->value = strdup(tempvalues[i]);
  	h->next = request->dataheaders;
  	request->dataheaders = h;
  }
  MyFree(tempheaders);
  MyFree(tempvalues);
  return 1;
}

/* Normal copyright applies here */

char	*GetField(HTTPd_Header *header, char *name)
{
	HTTPd_Header *p;
	for (p = header; p; p = p->next)
		if (!strcasecmp(p->name, name))
			return (p->value);
	return NULL;
	
}

char	*GetHeader(HTTPd_Request *request, char *name)
{
	HTTPd_Header *p;
	for (p = request->headers; p; p = p->next)
		if (!strcasecmp(p->name, name))
			return (p->value);
	return NULL;
}

void 	httpd_standard_headerX(HTTPd_Request *request, char *type, int extra)
{
	char		 datebuf[100];
	sockprintf(request, "HTTP/1.1 200 OK");
	sockprintf(request, "Server: UnrealIRCd HTTPd");
	sockprintf(request, "Connection: close");
	sockprintf(request, "Date: %s", (char *) rfctime(time(NULL), datebuf));
	sockprintf(request, "Content-Type: %s", type);
	if (extra != 1)	
		sockprintf(request, "");
}

void 	httpd_standard_header(HTTPd_Request *request, char *type)
{
	httpd_standard_headerX(request, type, 0);
}

void 	httpd_404_header(HTTPd_Request *request, char *path)
{
	char		 datebuf[100];
	sockprintf(request, "HTTP/1.1 404 Not Found");
	sockprintf(request, "Server: UnrealIRCd HTTPd");
	sockprintf(request, "Connection: close");
	sockprintf(request, "Date: %s", (char *) rfctime(time(NULL), datebuf));
	sockprintf(request, "Content-Type: text/html");
	sockprintf(request, "");
	sockprintf(request, "<b>Not Found</b><br>Could not find %s<br>",
		path);
}

void 	httpd_304_header(HTTPd_Request *request)
{
	char		 datebuf[100];
	sockprintf(request, "HTTP/1.1 304 Not Modified");
	sockprintf(request, "Server: UnrealIRCd HTTPd");
	sockprintf(request, "Connection: close");
	sockprintf(request, "Date: %s", (char *) rfctime(time(NULL), datebuf));
	sockprintf(request, "Content-Type: text/html");
	sockprintf(request, "");
}


void 	httpd_500_header(HTTPd_Request *request, char *why)
{
	char		 datebuf[100];
	sockprintf(request, "HTTP/1.1 500 Internal Server Error");
	sockprintf(request, "Server: UnrealIRCd HTTPd");
	sockprintf(request, "Connection: close");
	sockprintf(request, "Date: %s", (char *) rfctime(time(NULL), datebuf));
	sockprintf(request, "Content-Type: text/html");
	sockprintf(request, "");
	sockprintf(request, "%s<br>",
		why);
}

void	httpd_parse_final(HTTPd_Request *request)
{
	if (request->method == 0)
	{
		char	*cl;
		
		cl = GetHeader(request, "Content-Length:");
		if (!cl)
		{
			sockprintf(request, "Missing content-length header..");
			return;
		}
		request->content_length = atoi(cl);
		if (request->content_length <= 1023)
		{
			request->state = 2;
		}
	}
	else
	{
		RunHookReturnVoid(HOOKTYPE_HTTPD_URL, request, >0);
		httpd_404_header(request, request->url);
		return;
	}
}

void	httpd_sendfile(HTTPd_Request *r, char *filename)
{
	int	xfd = open(filename, O_RDONLY);
	int	len = 0, ret = 0;
	
	if (xfd)
	{
		while ((len = read(xfd, &r->inbuf[0], 1000)))
		{
			ret = send(r->fd, &r->inbuf[0], len, 0);
			if (ret == -1)
			{
				ircd_log(LOG_ERROR, "send error: %s",
					strerror(ERRNO));
				goto iwilldie;
			}
			if (ret != len)
			{
				ircd_log(LOG_ERROR, "%i != %i",
					ret, len);
				goto iwilldie;
			}
		}
	}
	iwilldie:
	close(xfd);
	return;
}
