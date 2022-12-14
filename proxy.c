#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
// 만들어 질 수 있는 캐시 개수
// MAX_CACHE_SIZE/MAX_OBJECT_SIZE
#define CACHE_OBJS_COUNT 10
// 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법
#define LRU_MAGIC_NUMBER 9999

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

// commnuication from client to server
void doit(int connfd);
// parsing the uri that client requests
void parse_uri(char *uri,char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
// int connect_endServer(char *hostname, int port, char *http_header);
int connect_endServer(char *hostname, int port);

// 쓰레드 함수 선언
void *thread(void *vargp);

/* 프록시 구현 과정
    1. 클라이언트와의 fd를 클라이언트용 rio에 연결(Rio_readinitb)함
    a. rio는 Registered Input/Output 라는 소켓 API 임 - 메시지를 보낼 수 있는 텍스트창으로 보면 됨.
    2. 클라이언트의 요청을 한줄 읽어들여서(Rio_readlineb) 메서드와 URI, HTTP 버전을 얻고, URI에서 목적지 호스트와 포트를 뽑아냄
    3. 목적지 호스트와 포트를 가지고 서버용 fd를 생성하고, 서버용 rio에 연결(Rio_readinitb)함
    4. 클라이언트가 보낸 첫줄을 이미 읽어 유실되었고, HTTP 버전을 바꾸거나 추가 헤더를 붙일 필요가 있음
        → 클라이언트가 보내는 메시지를 한줄씩 읽어들이면서(Rio_readlineb) 재조합하여 서버에 보낼 HTTP 요청메시지를 새로 생성해줌
    5. 서버에 요청메시지를 보냄(Rio_writen)
    6. 서버 응답이 오면 클라이언트에게 전달함 (Rio_readnb, Rio_writen)
*/

// 캐시를 위한 구현 함수들
void cache_init();

void cache_uri(char *uri, char *buf);
int cache_eviction();

int cache_find(char *url);
void readerPre(int i);
void readerAfter(int i);

void cache_LRU(int index);

void writePre(int i);
void writeAfter(int i);

// 캐시 블럭 구조체
typedef struct 
{
    char cache_obj[MAX_OBJECT_SIZE];
    char cache_url[MAXLINE];
    // least recently used 가장 최근에 사용한 것의 우선순위를 뒤로 미룸
    // 값이 낮은 값부터 먼저 삭제
    // 캐시에서 삭제할 때 사용
    int LRU;
    // 이 블럭에 캐시 정보가 들었는지 empty인지 아닌지 체크
    // 캐시 할당 여부 체크
    int isEmpty;

    // 읽는 개수
    // 누군가 읽고(사용하고) 있으면 1
    int readCnt;
    // 캐시에 접근하는 것을 프로텍트해주는 뮤텍스
    // 세마포어 타입 - 1:사용가능, 0:사용불가능
    sem_t wmutex;
    // rdcntmutex(read count mutex)
    // 리드카운트에 접근하는걸 프로텍트해주는 뮤텍스
    sem_t rdcntmutex;
}cache_block;

// 캐시 넘버 구조체
typedef struct
{
    // 10개 캐시 블록 부여
    cache_block cacheobjs[CACHE_OBJS_COUNT];
}Cache;

Cache cache;

int main(int argc, char **argv)
{
    // malloc을 사용하므로 connfd에 * 추가
    int listenfd, *connfd;

    socklen_t  clientlen;
    char hostname[MAXLINE], port[MAXLINE];

    struct sockaddr_storage clientaddr;/*generic sockaddr struct which is 28 Bytes.The same use as sockaddr*/

    pthread_t tid;

    // port number가 argument로 입력되지 않은 경우 error반환
    if(argc != 2){
        fprintf(stderr,"usage :%s <port> \n",argv[0]);
        exit(1);
    }

    // 클라이언트가 정상적이지 않은 방법으로 종료되었을 경우
    // 서버에서 그 소켓에 접근하여 writen하려고 하면 잘못됐다는 response(Signal)를 보냄
    // 이 프로세스는 다른 클라이언트들과도 연결되어 있기때문에 시그널을 무시해야함
    // SIG_IGN : signal ignore
    Signal(SIGPIPE, SIG_IGN);

    // get listenfd
    listenfd = Open_listenfd(argv[1]);
    while(1) {
        clientlen = sizeof(clientaddr);

        // 쓰레드 경쟁 상태를 피하기 위한 동적 할당
        connfd = Malloc(sizeof(int));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        // hostname과 portnumber string으로 반환
        Getnameinfo((SA*)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s).\n", hostname, port);

        // 쓰레드 생성
        // 첫 번째 인자 *thread: 쓰레드 식별자 / 두 번째: 쓰레드 특성 지정 (기본: NULL) / 세 번째: 쓰레드 함수 / 네 번째: 쓰레드 함수의 매개변수
        Pthread_create(&tid, NULL, thread, (void *)connfd);

        // doit(connfd);
        // Close(connfd);
    }

    return 0;
}

/* Thread routine */
// 쓰레드 함수 정의
void *thread(void *vargp){
    int connfd = *((int *)vargp);

    doit(connfd);
    Pthread_detach(Pthread_self());

    Close(connfd);

    return NULL;
}

void doit(int connfd)
{
    // proxy 뒤에 존재하는 end server
    int end_serverfd;
    int port;

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_header[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    /*rio is client's rio,server_rio is endserver's rio*/
    rio_t rio, server_rio;

    Rio_readinitb(&rio, connfd);
    // read the client's rio into buffer
    Rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf,"%s %s %s", method, uri, version);
    // request의 method가 GET이 아니면 error 처리
    if(strcasecmp(method, "GET")){
        printf("Proxy does not implement the method");
        return;
    }

    // 캐시에서 uri를 사용해야 하므로
    // doit으로 받아온 connfd가 들고있는 uri를 넣어줌
    char url_store[100];
    strcpy(url_store, uri);

    // 캐시 확인 여부
    int cache_index;
    // in cache then return the cache content
    // cache_index정수 선언
    // url_store에 있는 인덱스를 뒤짐(chche_find:10개의 캐시블럭)
    // 찾아서 나온 인덱스가 -1이 아닌 경우, 캐시가 존재하므로 해당 캐시 인덱스에 접근
    if ((cache_index=cache_find(url_store)) != -1) {
        // 캐시 뮤텍스를 풀어줌 (0->1)
        readerPre(cache_index);
        // 캐시에서 찾은 값을 connfd에 쓰고, 캐시에서 그 값을 바로 보내게 됨
        // cache.cacheobjs[cache_index].cache_obj에서 cache.cacheobjs[cache_index].cache_obj의 길이만큼 connfd에 써라
        Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
        // 닫아줌 1->0
        readerAfter(cache_index);
        return;
    }


    parse_uri(uri, hostname, path, &port);
    /*build the http header which will send to the end server*/
    build_http_header(endserver_http_header, hostname, path, port, &rio);

    /*connect to the end server*/
    // end_serverfd = connect_endServer(hostname, port, endserver_http_header);
    end_serverfd = connect_endServer(hostname, port);
    if(end_serverfd < 0) {
        printf("connection failed\n");
        return;
    }

    Rio_readinitb(&server_rio, end_serverfd);
    /*write the http header to endserver*/
    Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header));

    /*receive message from end server and send to the client*/
    // 캐시가 없는 경우
    // proxy거쳐서 서버에서 response오는데, 그 응답을 캐시로 저장하고 클라이언트에 보내는 과정
    char cachebuf[MAX_OBJECT_SIZE];
    int sizebuf = 0;
    
    size_t n;
    // while((n=Rio_readlineb(&server_rio,buf,MAXLINE))!=0)
    // {
    //     printf("proxy received %d bytes,then send\n",n);
    //     Rio_writen(connfd,buf,n);
    // }

    // 서버에서 응답하는 내용이 0이 아닌 경우
    while ((n=Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
        // printf("proxy received %d bytes,then send\n",n);

        sizebuf += n;
        // 캐시는 MAX_OBJECT_SIZE 만큼 저장할 수 있으므로
        // MAX_OBJECT_SIZE 보다 작으면
        if (sizebuf < MAX_OBJECT_SIZE)
            // cachebuf에 but(response값) 다 이어붙혀놓음(캐시내용)
            strcat(cachebuf, buf);

        Rio_writen(connfd, buf, n);
    }
    Close(end_serverfd);

    // store it
    if (sizebuf < MAX_OBJECT_SIZE) {
        // url_store에 cachebuf 저장
        cache_uri(url_store, cachebuf);
    }
}

//http_header 인자에 만들어서 반환
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
    // request_hdr에 reqquestlint_hdr_format을 담음(path인자는 reqquestlint_hdr_format에 들어갈 값)
    // path는 request source 경로
    sprintf(request_hdr, requestlint_hdr_format, path);
    /*get other request header for client rio and change it */
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        // 읽어들인 값(buf)가 /r/n이면 break
        if(strcmp(buf, endof_hdr) == 0)
        {
            break;
        }

        // host header값 만들어주기
        if(!strncasecmp(buf, host_key, strlen(host_key)))/*Host:*/
        {
            // buf를 host_hdr에서 지정한 위치로 복사
            strcpy(host_hdr, buf);
            continue;
        }

        // host header 이외의 header 만들어주기
        if(!strncasecmp(buf, connection_key, strlen(connection_key))
                &&!strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
                &&!strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_hdr, buf);
        }
    }

    // request header에 host header가 없다면 hostname으로 만들어주기
    if(strlen(host_hdr) == 0)
    {
        sprintf(host_hdr, host_hdr_format, hostname);
    }
    // 완전체 만들어주기
    sprintf(http_header, "%s%s%s%s%s%s%s", request_hdr, host_hdr, conn_hdr, prox_hdr, user_agent_hdr, other_hdr, endof_hdr);
    return ;
}

/*Connect to the end server*/
inline int connect_endServer(char *hostname, int port){
    char portStr[100];
    // portstr에 port 넣어주기
    sprintf(portStr, "%d", port);
    // 해당 hostname과 portStr로 end_server에게 가는 요청만들어주기
    return Open_clientfd(hostname, portStr);
}

/*parse the uri to get hostname,file path ,port*/
void parse_uri(char *uri,char *hostname,char *path,int *port)
{
    *port = 80;
    // uri에서 "//"의 첫 번째 표시 시작 위치에 대한 포인터를 리턴
    char* pos = strstr(uri, "//");

    pos = pos != NULL? pos+2:uri;

    char*pos2 = strstr(pos, ":");
    if(pos2 != NULL)
    {
        *pos2 = '\0';
        sscanf(pos, "%s", hostname);
        sscanf(pos2+1, "%d%s", port, path);
    }
    else
    {
        pos2 = strstr(pos,"/");
        if(pos2!=NULL)
        {
            *pos2 = '\0';
            sscanf(pos,"%s",hostname);
            *pos2 = '/';
            sscanf(pos2,"%s",path);
        }
        else
        {
            sscanf(pos,"%s",hostname);
        }
    }
    return;
}

// 캐시 함수 구현
// 캐시 초기화
void cache_init() {
    // 처음이니까 0으로 설정
    cache.cache_num = 0;

    int i;
    for (i=0; i<CACHE_OBJS_COUNT; i++) {
        // LRU : 우선 순위를 미는 것
        // 아무 것도 없으므로 모든 우선 순위를 0으로 설정
        cache.cacheobjs[i].LRU = 0;
        // 1이 비어있다는 뜻
        cache.cacheobjs[i].isEmpty = 1;

        // Sem_init : 세마포어 초기화 함수(초기화할 세마포어의 포인터, 세마포어 공유 여부, 초기 값
        // 세마포어 공유 여부 => 0 - 쓰레드들끼리 세마포어 공유, 그 외 - 프로세스 간 공유
        // 현재는 쓰레드에서 사용 중이므로 0으로 설정
        Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);
        Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1); 
        
        // read count를 0으로 놓고 init을 끝냄
        cache.cacheobjs[i].readCnt = 0;
    }
}

// 캐시에 URI 및 컨텐츠를 저장
void cache_uri(char *uri, char *buf) {
    // 가장 처음으로 찾는 빈 캐시 블럭의 인덱스
    int i = cache_eviction();
    
    writePre(i);

    // 저장해야할 내용을 캐시 블럭에 저장
    strcpy(cache.cacheobjs[i].cache_obj, buf);
    strcpy(cache.cacheobjs[i].cache_url, uri);

    // 캐시를 저장하는 것이므로 0으로 비어있지 않음을 표시
    cache.cacheobjs[i].isEmpty = 0;
    // 가장 최근에 했으니 우선순위 9999로 설정
    cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;

    // 새로 저장된 블럭의 인덱스 값을 넘겨줌
    cache_LRU(i);

    writeAfter(i);
}

// 
int cache_eviction() {
    // 제일 마지막에 들어오는 들어오는 캐시이므로 LRU_MAGIC_NUMBER인 9999 할당
    int min = LRU_MAGIC_NUMBER;
    // 지워줘야할 인덱스
    // 아직 비교 전이라 0으로 설정
    int minindex = 0;

    int i;
    // 블럭의 개수만큼 돌면서 검사
    for (i=0; i<CACHE_OBJS_COUNT; i++) {
        readerPre(i);

        // 만약 해당 인덱스의 캐시가 비어있을 경우
        if (cache.cacheobjs[i].isEmpty == 1) {
            // 비어있는 곳을 찾았으므로 해당 인덱스를 지워줘야할 인덱스로 저장
            minindex = i;
            // 할당 해제
            readerAfter(i);
            break;
        }

        // 해당 인덱스의 우선순위가 현재 저장된 우선 순위보다 작을 경우
        if (cache.cacheobjs[i].LRU < min) {
            // 먼저 지워주어야 하므로 minindex 값을 해당 인덱스의 값으로 바꿈
            minindex = i;
            // 그 다음 비교를 위해 min 값을 해당 인덱스의 LRU로 변경
            min = cache.cacheobjs[i].LRU;
            // 할당 해제
            readerAfter(i);
            continue;
        }
        // 할당 해제
        readerAfter(i);
    }
    // 지워줄 인덱스 반환
    return minindex;
}

void writePre(int i) {
    P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i) {
    V(&cache.cacheobjs[i].wmutex);
}

// i = 해당 인덱스
// 내가 받아온 index오브젝트의 리드카운트 뮤텍스를 비어있는지 확인하고
// 비었다면 할당 받음
void readerPre(int i) {
    // P연산(locking):정상인지 검사, 기다림 - 비정상인 경우 에러 출력
    P(&cache.cacheobjs[i].rdcntmutex);
    // rdcntmutex로 특정 readcnt에 접근하고 +1해줌
    // 원래 0으로 세팅되어있어서, 누가 안쓰고 있으면 0이었다가 1로 되고 if문 들어감
    cache.cacheobjs[i].readCnt++;

    // readerAfter에서 -1 다시 내려주기때문에 0, 1, 0 에서만 움직임
    if (cache.cacheobjs[i].readCnt == 1)
        // mutex 뮤텍스를 풀고(캐시에 접근)
        P(&cache.cacheobjs[i].wmutex);

    // V연산 풀기(캐시 해제)
    V(&cache.cacheobjs[i].rdcntmutex);
}

// 할당 받았던 뮤텍스를 다시 풀어주는 함수
void readerAfter(int i) {
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt--;

    if (cache.cacheobjs[i].readCnt == 0)
        V(&cache.cacheobjs[i].wmutex);

    V(&cache.cacheobjs[i].rdcntmutex);
}

void cache_LRU(int index) {
    int i;
    for (i = 0; i < CACHE_OBJS_COUNT; i++) {
        // 해당 인덱스와 같으면 새로 저장한 내용이므로
        // continue로 넘어감
        if (i == index) {
            continue;
        }

        writePre(i);

        // 이외의 나머지는 할당 해제 상태로 만들고
        // 우선순위를 뒤로 미룸
        if (cache.cacheobjs[i].isEmpty == 0) {
            cache.cacheobjs[i].LRU--;
        }

        writeAfter(i);
    }
}

// 찾는 캐시가 있는지 찾아보는 함수
int cache_find(char *url) {
  int i;
  
  // 캐시의 총 개수만큼 돌면서 검사
  for (i = 0; i < CACHE_OBJS_COUNT; i++) {
    // 현재 찾는 인덱스의 캐시가 사용중인지 검사후 할당 받음
    readerPre(i);

    // 해당 인덱스의 캐시가 비어있지 않으면서 찾고있는 url과 같은 url을 가지고 있는 경우
    if (cache.cacheobjs[i].isEmpty == 0 && strcmp(url, cache.cacheobjs[i].cache_url) == 0) {
        // 할당 받았던 캐시를 풀어주고
        readerAfter(i);
        // 찾은 인덱스 반환
        return i;
    }
    readerAfter(i);
  }
  // 찾고 있는 내용이 캐시에 없으므로 -1 반환
  return -1;
}