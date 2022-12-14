/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char* method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char* method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void wait_childproc(int sig);

int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  Signal(SIGCHLD, wait_childproc);  // 자식 프로세스 종료시(SIGCHLD) Wait 함수를 호출 하는 함수 등록
  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen); // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);  // line:netp:tiny:doit
    Close(connfd); // line:netp:tiny:close
  }
}

/*
  TINY doit은 한 개의 HTTP 트랜잭션을 처리한다.
*/
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /* Read request line and headers */
  Rio_readinitb(&rio, fd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request heards:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  // GET 요청 읽고 처리
  // 숙제 5(11.11)_1 - HEAD 요청 읽고 처리 추가
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")){
    // 501은 해당 method를 지원하지 않는다는 의미.
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }

  read_requesthdrs(&rio);

  /* Parse URI from GET request */
  /* is_static은 이 요청이 정적인 요청인가(단순 작성된 파일을 달라는 것인가 아니면 요청에 맞게 조작된 무언갈 원하는가) */
  is_static = parse_uri(uri, filename, cgiargs);

  // stat은 파일의 크기, 파일의 권한, 파일의 생성일시, 파일의 최종 변경일 등, 파일의 상태나 파일의 정보를 얻는 함수.
  // 없다면 -1을 반환, 있다면 sbuf에 담긴다.
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  /* Serve static content */
  if (is_static)
  {
    /*
      st_mode에는 파일의 종류, 파일의 퍼미션을 알 수 있는 정보가 담김.
      각각 정규 파일인지, 사용자가 읽을 수 있는 파일인지 판별
      그 정보는 sbuf에 담겨져 있다.
    */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size, method);
  }
  else /* Serve dynamic content */
  { 
    /*
      각각 정규 파일인지, 사용자가 쓸 수 있는 파일인지 판별
      그 정보는 sbuf에 담겨져 있다.
    */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) 
    {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs, method);
  }

}

/*
  에러 페이지 생성하는 메서드
*/
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /* Build the HTTP response body -> body에 저장됨*/
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n" , body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response, 클라이언트에 전송*/
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  // 클라 파일 디스크립터에 전송(쓴다)
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));

  // 작성한 response 페이지를 전송
  Rio_writen(fd, body, strlen(body));
  
}

/* 요청의 헤더를 읽어서 rio 구조체에 담아준다. */
/* 하지만.. TINY read_req니esthdrs 요청 헤더를 읽고 무시한다. */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  // rp에 클라 소켓 들어가있고, 클라가 보낸 데이터를 buf에 저장해준다.
  Rio_readlineb(rp, buf, MAXLINE);

  // 헤더는 하나의 개행으로 바디와 헤더를 구분한다.
  while(strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/*
  Tiny는 정적 컨텐츠를 위한 홈 디렉토리가 자신의 현재 디렉토리이고,
  실행파일의 홈 디렉토리는 /cgl-bin이라고 가정한다.
  스트링 cgi-bln을 포함하는 모든 URI는 동적 컨텐츠를 요청하는 것을 나타낸다고 가정한다. 
  기본 파일 이름은 ./home.html이다
*/
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  // strstr은 uri에서 "cgi-bin"을 찾는다. 못찾으면 NULL, 즉, NULL을 반환하면 정적인 콘텐츠 요청이다.
  // 정적 콘텐츠 요청은 1을 반환 -> is_static에 저장된다.
  if (!strstr(uri, "cgi-bin"))
  { /* Static content  ./index.html 같은 상대 리눅스 경로 이름으로 변환*/ 
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri);
    if (uri[strlen(uri)-1]=='/')
      strcat(filename, "home.html");
    return 1;
  }
  else
  { /* Dynamic content */
    // 쿼리가 시작되는 인덱스를 반환
    ptr = index(uri, '?');
    if (ptr)
    { 
      // '?' 이후의 값을 가져와서 cgiargs에 붙힌다.
      strcpy(cgiargs, ptr+1);
      *ptr = '\0';
    }
    else
    { /* 쿼리가 없다면..*/
      strcpy(cgiargs, "");
    }
    strcpy(filename, ".");
    strcat(filename, uri);
    return 0;
  }
}

/* Tiny serve_static은 정적 컨텐츠를 클라이언트에게 서비스한다. */
void serve_static(int fd, char *filename, int filesize, char* method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Send response headers to client */
  // 파일 이름에서 확장자를 찾아온다.
  get_filetype(filename, filetype);

  sprintf(buf, "HTTP/1.0 200 0K\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sCormection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf , filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  // 숙제 5(11.11)_2
  // 정적 HEAD 처리, method 받아올 것
  if (!strcasecmp(method, "HEAD")){
    return;
  }

  /* Send response body to client */
  // 해당 디렉토리 내에서 filename의 파일을 찾아서(읽기 전용) 디스크립터 반환 
  // 파일을 메모리에 매핑시킨다.
  srcfd = Open(filename, O_RDONLY, 0);

  // 숙제 3번(11.9)
  void *src_buf = malloc(filesize);
  Rio_readn(srcfd, src_buf, filesize);
  Close(srcfd);
  Rio_writen(fd, src_buf, filesize);
  free(src_buf);
  
  /*srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    Close(srcfd);
    // 두번째 인자에 기존에 버퍼를 넣어주던것 대신, 읽어온 파일을 보낸다.
    Rio_writen(fd, srcp, filesize);
    Munmap(srcp, filesize);
  */
}

/*
* get_filetype — Derive file type from filename
*/
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  // 숙제 2번(11.7)
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");  
  else if (strstr(filename, ".mpg"))
    strcpy(filetype, "video/mpeg");
  else
    strcpy(filetype, "text/plain");
}

/* Tiny serve_dynamic은 동적콘텐츠를 클라이언트에 제공한다. */
void serve_dynamic(int fd, char *filename, char *cgiargs, char* method)
{
  char buf[MAXLINE], *emptylist[] = { NULL };
  /* Return first part of HTTP response */
  sprintf(buf, "HTTP/1.0 200 0K\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  // 숙제 5(11.11)_3
  // 동적 HEAD 처리, method 받아올 것
  if (!strcasecmp(method,"HEAD")){
    return;
  }

  // 자식 프로세스를 생성
  if (Fork() == 0) 
  { /* Child */

    /* Real server would set all CGI vars here */
    /* cgiargs에는 쿼리스트링 값(파일 이름 ex) home.html)이 들어가있다.
       QUERY_STRING 값으로 addr 같은 걸 넣어준다. -> getenv("QUERY_STRING") 하면 해당 실행파일의 이름이 반환
    */
    setenv("QUERY_STRING", cgiargs, 1);
    Dup2(fd, STDOUT_FILENO);  /* Redirect stdout to client */

    /* Execve의 두 번째 인재는 파일이 실행될 때 ./filename {이 쪽에 들어가는 인자}  */
    /* Execve의 세 번째 인재는 파일이 실행될 때 해당 실행파일의 환경변수로 들어감 */
    Execve(filename, emptylist, environ); /* Run CGI program */
  }
  //Wait(NULL); /* Parent waits for and reaps child */
}

/*
 * 자식 프로세스가 종료되면 소멸 시킨다.
 */
void wait_childproc(int sig)
{
  pid_t pid;
  int status;
  pid = Wait(&status);
}
