#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
// ---------------- Windows ----------------
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

static SOCKET socket_tcp = INVALID_SOCKET;
static HANDLE hilo_receptor;

// Hilo que escucha frames TCP recibidos
DWORD WINAPI escuchar_mensajes(LPVOID arg) {
    SOCKET s = *(SOCKET*)arg;
    for (;;) {
        // --- recv_frame_tcp ---
        uint32_t be_len = 0;
        int got = 0;
        while (got < 4) {
            int r = recv(s, (char*)&be_len + got, 4 - got, 0);
            if (r == 0)  return 0;              // broker cerró
            if (r == SOCKET_ERROR) return 0;    // error
            got += r;
        }
        uint32_t len = ntohl(be_len);
        if (len == 0 || len > (64u * 1024u)) return 0;

        char* buf = (char*)malloc(len + 1);
        if (!buf) return 0;

        got = 0;
        while (got < (int)len) {
            int r = recv(s, buf + got, (int)len - got, 0);
            if (r == 0 || r == SOCKET_ERROR) { free(buf); return 0; }
            got += r;
        }
        buf[len] = '\0';

        if (buf[0] != '\0') {
            printf("\nMensaje recibido: %s\n> ", buf); fflush(stdout);
        }
        free(buf);
    }
    return 0;
}

// Manejador Ctrl+C
BOOL WINAPI manejar_ctrl_c(DWORD evento){
    if (evento == CTRL_C_EVENT || evento == CTRL_BREAK_EVENT){
        printf("Cerrando el socket y terminando la ejecucion...\n");
        if (socket_tcp != INVALID_SOCKET) {
            shutdown(socket_tcp, SD_BOTH);
            closesocket(socket_tcp);
            socket_tcp = INVALID_SOCKET;
        }
        if (hilo_receptor) CloseHandle(hilo_receptor);
        WSACleanup();
        ExitProcess(0);
    }
    return FALSE;
}

#else
// ---------------- Linux/Unix ----------------
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

static int socket_tcp = -1;
static pthread_t hilo_receptor;

// Hilo que escucha frames TCP recibidos
void* escuchar_mensajes(void* arg) {
    int s = *(int*)arg;
    for (;;) {
        // --- recv_frame_tcp ---
        uint32_t be_len = 0;
        int got = 0;
        while (got < 4) {
            int r = recv(s, ((char*)&be_len) + got, 4 - got, 0);
            if (r == 0)  return NULL;          // broker cerró
            if (r < 0)   return NULL;          // error
            got += r;
        }
        uint32_t len = ntohl(be_len);
        if (len == 0 || len > (64u * 1024u)) return NULL;

        char* buf = (char*)malloc(len + 1);
        if (!buf) return NULL;

        got = 0;
        while (got < (int)len) {
            int r = recv(s, buf + got, (int)len - got, 0);
            if (r == 0 || r < 0) { free(buf); return NULL; }
            got += r;
        }
        buf[len] = '\0';

        printf("\nMensaje recibido: %s\n> ", buf); fflush(stdout);
        free(buf);
    }
    return NULL;
}

// Ctrl+C
void manejar_ctrl_c(int signo){
    (void)signo;
    printf("Cerrando el socket y terminando la ejecucion...\n");
    if (socket_tcp >= 0) {
        shutdown(socket_tcp, SHUT_RDWR);
        close(socket_tcp);
        socket_tcp = -1;
    }
    // no se cierra el pthread_t con close(); el hilo termina al cerrar el socket
    exit(0);
}
#endif

// --------------------------------------------------------
// Función que lee el tópico del usuario y construye "SUB|topico|."
// --------------------------------------------------------
static char* leer_mensaje(void) {
    char* mensaje   = (char*)malloc(160);
    char* contenido = (char*)malloc(151);
    if (!mensaje || !contenido) {
        printf("Error al asignar memoria.\n");
        free(mensaje); free(contenido);
        return NULL;
    }

    printf("Ingrese el topico al que desea suscribirse: \n");
    if (!fgets(contenido, 151, stdin)) {
        free(mensaje); free(contenido);
        return NULL;
    }
    int len = (int)strlen(contenido);
    if (len > 0 && contenido[len-1] == '\n') { contenido[--len] = '\0'; }

    // "SUB|<topico>|."
    memcpy(mensaje, "SUB|", 4);
    memcpy(mensaje + 4, contenido, len);
    mensaje[4 + len] = '|';
    mensaje[5 + len] = '.';
    mensaje[6 + len] = '\0';

    free(contenido);
    return mensaje;
}

// -------- utilidades de envío con framing --------
static int send_all_tcp(
#ifdef _WIN32
    SOCKET s,
#else
    int s,
#endif
    const char* buf, int len)
{
    if (!buf || len < 0) return -1;
    int sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int r = send(s, buf + sent, len - sent, 0);
        if (r == SOCKET_ERROR) return -1;
#else
        int r = send(s, buf + sent, len - sent, 0);
        if (r < 0) return -1;
#endif
        if (r == 0) return -1;
        sent += r;
    }
    return sent;
}

static int send_frame_tcp(
#ifdef _WIN32
    SOCKET s,
#else
    int s,
#endif
    const char* payload, int len)
{
    if (!payload || len < 0) return -1;
    if (len > 0x7FFFFFFF) return -1;
    uint32_t be = htonl((uint32_t)len);
    if (send_all_tcp(s, (const char*)&be, 4) < 0) return -1;
    if (send_all_tcp(s, payload, len) < 0) return -1;
    return 4 + len;
}

// --------------------------------------------------------
// Función principal
// --------------------------------------------------------
int main(){
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0){
        printf("Error al iniciar Winsock.\n");
        return 1;
    }
    SetConsoleCtrlHandler(manejar_ctrl_c, TRUE);
#else
    signal(SIGINT, manejar_ctrl_c);
    signal(SIGPIPE, SIG_IGN); // no morir por SIGPIPE al enviar
#endif

    // Solicitar IP/puerto del broker
    char* ip = (char*)malloc(16);
    char* puerto = (char*)malloc(6);
    if (!ip || !puerto){
        printf("Error al asignar memoria para IP/puerto.\n");
        free(ip); free(puerto);
        return 1;
    }
    printf("Ingrese la direccion IP del broker: \n");
    scanf("%15s", ip);
    printf("Ingrese el puerto del broker: \n");
    scanf("%6s", puerto);
    getchar(); // drenar '\n'

    // Resolver y conectar TCP
    struct sockaddr_in broker;
    memset(&broker, 0, sizeof(broker));
    broker.sin_family = AF_INET;
    broker.sin_port   = htons( (unsigned short)atoi(puerto) );
#ifdef _WIN32
    InetPton(AF_INET, ip, &broker.sin_addr);
#else
    inet_pton(AF_INET, ip, &broker.sin_addr);
#endif
    free(ip); free(puerto);

#ifdef _WIN32
    socket_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_tcp == INVALID_SOCKET){
        printf("Error al crear el socket TCP.\n");
        WSACleanup();
        return 1;
    }
    if (connect(socket_tcp, (struct sockaddr*)&broker, sizeof(broker)) == SOCKET_ERROR){
        printf("Error al conectar con el broker.\n");
        closesocket(socket_tcp); WSACleanup();
        return 1;
    }
#else
    socket_tcp = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_tcp < 0){
        perror("socket");
        return 1;
    }
    if (connect(socket_tcp, (struct sockaddr*)&broker, sizeof(broker)) < 0){
        perror("connect");
        close(socket_tcp);
        return 1;
    }
#endif

    // Crear hilo receptor (lee frames y los imprime)
#ifdef _WIN32
    hilo_receptor = CreateThread(NULL, 0, escuchar_mensajes, &socket_tcp, 0, NULL);
    if (hilo_receptor == NULL) {
        printf("Error al crear el hilo receptor.\n");
        closesocket(socket_tcp); WSACleanup();
        return 1;
    }
#else
    if (pthread_create(&hilo_receptor, NULL, escuchar_mensajes, (void*)&socket_tcp) != 0){
        perror("pthread_create");
        close(socket_tcp);
        return 1;
    }
#endif

    // Bucle principal: leer tópico y enviar SUB por TCP (con framing)
    while (1) {
        char* mensaje = leer_mensaje();
        if (mensaje != NULL) {
            int r = send_frame_tcp(socket_tcp, mensaje, (int)strlen(mensaje));
#ifdef _WIN32
            if (r < 0) printf("Error al enviar la suscripcion.\n");
#else
            if (r < 0) perror("envio suscripcion");
#endif
            free(mensaje);
        }
        // prompt
        printf("> "); fflush(stdout);
    }

    // (Nunca se llega aquí normalmente)
#ifdef _WIN32
    shutdown(socket_tcp, SD_BOTH);
    closesocket(socket_tcp);
    WSACleanup();
#else
    shutdown(socket_tcp, SHUT_RDWR);
    close(socket_tcp);
#endif
    return 0;
}
