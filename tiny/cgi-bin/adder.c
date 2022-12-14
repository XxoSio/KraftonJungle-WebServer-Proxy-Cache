/*
 * adder.c - a minimal CGI program that adds two numbers together
 *         - 두 개의 숫자를 더하는 최소 CGI 프로그램
 */
/* $begin adder */
#include "csapp.h"

int main(void) {
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1=0, n2=0;

    /* Extract the two arguments */
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        // 기본 코드
        // strcpy(arg1, buf);
        // strcpy(arg2, p+1);
        // n1 = atoi(arg1);
        // n2 = atoi(arg2);

        // 숙제4번(11.10)
        sscanf(buf, "num1=%d", &n1);
        sscanf(p+1, "num2=%d", &n2);
    }

    /* Make the response body */
    sprintf(content, "Welcome to add.com: ");
    sprintf(content, "%sTHE Internet addition portal.\r\n<p>", content);
    sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>", 
    content, n1, n2, n1 + n2);
    sprintf(content, "%sThanks for visiting!\r\n", content);
  
    /* Generate the HTTP response */
    // method가 GET일 경우에만 reponse를 보냄
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);

    exit(0);
}
/* $end adder */
