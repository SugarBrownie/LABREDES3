
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

static SOCKET socket_udp = INVALID_SOCKET;
static HANDLE hilo_receptor;
static volatile LONG need_resub = 0; 

DWORD WINAPI escuchar_mensajes(LPVOID arg);
BOOL WINAPI manejar_ctrl_c(DWORD evento){
    if (evento == CTRL_C_EVENT || evento == CTRL_BREAK_EVENT){
        printf("\nCerrando el socket y terminando la ejecucion...\n");
        if (socket_udp != INVALID_SOCKET) {
            closesocket(socket_udp);
            socket_udp = INVALID_SOCKET;
        }
        if (hilo_receptor) CloseHandle(hilo_receptor);
        WSACleanup();
        ExitProcess(0);
    }
    return FALSE;
}

#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

static int socket_udp = -1;
static pthread_t hilo_receptor;
static volatile int need_resub = 0;   

void* escuchar_mensajes(void* arg);
void manejar_ctrl_c(int signo){
    (void)signo;
    printf("\nCerrando el socket y terminando la ejecucion...\n");
    if (socket_udp >= 0) {
        close(socket_udp);
        socket_udp = -1;
    }
    exit(0);
}
#endif

static void chomp(char* s){
    if (!s) return;
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = '\0';
}


static void imprimir_tema_contenido(const char* payload) {
    if (!payload || !*payload) return;

    // 1) PUB|tema|contenido
    if (strncmp(payload, "PUB|", 4) == 0) {
        const char* p = payload + 4;
        const char* bar1 = strchr(p, '|');
        if (bar1) {
            const char* p2 = bar1 + 1;
            const char* bar2 = strchr(p2, '|');
            if (bar2) {

            }
            char tema[200] = {0};
            size_t len_tema = (size_t)(bar1 - p);
            if (len_tema > sizeof(tema)-1) len_tema = sizeof(tema)-1;
            memcpy(tema, p, len_tema);
            const char* contenido = bar1 + 1;
            printf("Mensaje: %s|%s\n", tema, contenido);
            return;
        }
    }

    const char* bar = strchr(payload, '|');
    if (bar) {
        char tema[200] = {0};
        size_t len_tema = (size_t)(bar - payload);
        if (len_tema > sizeof(tema)-1) len_tema = sizeof(tema)-1;
        memcpy(tema, payload, len_tema);
        const char* contenido = bar + 1;
        printf("Mensaje: %s|%s\n", tema, contenido);
        return;
    }

    // 3) tema/contenido
    const char* slash = strchr(payload, '/');
    if (slash) {
        char tema[200] = {0};
        size_t len_tema = (size_t)(slash - payload);
        if (len_tema > sizeof(tema)-1) len_tema = sizeof(tema)-1;
        memcpy(tema, payload, len_tema);
        const char* contenido = slash + 1;
        printf("Mensaje: %s|%s\n", tema, contenido);
        return;
    }

    printf("Mensaje: %s\n", payload);
}


#ifdef _WIN32
DWORD WINAPI escuchar_mensajes(LPVOID arg) {
    (void)arg;
    char buf[1024];
    for (;;) {
        int r = recvfrom(socket_udp, buf, 1023, 0, NULL, NULL);
        if (r == SOCKET_ERROR) return 0;
        if (r <= 0) continue;
        buf[r] = '\0';

        if (strncmp(buf, "ERR|NO_TOPIC|", 13) == 0) {
            InterlockedExchange(&need_resub, 1);
            printf("\n[Broker] El tópico no existe. Vuelve a suscribirte.\n> ");
            fflush(stdout);
            continue;
        }

        imprimir_tema_contenido(buf);
        printf("> ");
        fflush(stdout);
    }
    return 0;
}
#else
void* escuchar_mensajes(void* arg) {
    (void)arg;
    char buf[1024];
    for (;;) {
        int r = (int)recvfrom(socket_udp, buf, 1023, 0, NULL, NULL);
        if (r < 0) return NULL;
        if (r == 0) continue;
        buf[r] = '\0';

        if (strncmp(buf, "ERR|NO_TOPIC|", 13) == 0) {
            need_resub = 1;
            printf("\n[Broker] El tópico no existe. Vuelve a suscribirte.\n> ");
            fflush(stdout);
            continue;
        }

        imprimir_tema_contenido(buf);
        printf("> ");
        fflush(stdout);
    }
    return NULL;
}
#endif

// --------------------------------------------------------
// Enviar SUB|<topico>|
// --------------------------------------------------------
static int enviar_sub(
#ifdef _WIN32
    SOCKET s,
#else
    int s,
#endif
    const struct sockaddr_in* broker,
    const char* topico)
{
    if (!broker || !topico || !*topico) return -1;
    char frame[256];
    int n = snprintf(frame, sizeof(frame), "SUB|%s|", topico);
    if (n <= 0 || n >= (int)sizeof(frame)) return -1;

    int r = sendto(s, frame, n, 0,
                   (const struct sockaddr*)broker, sizeof(*broker));
#ifdef _WIN32
    if (r == SOCKET_ERROR) return -1;
#else
    if (r < 0) return -1;
#endif
    return 0;
}

// --------------------------------------------------------
// main
// --------------------------------------------------------
int main(void){
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0){
        printf("Error al iniciar Winsock.\n");
        return 1;
    }
    SetConsoleCtrlHandler(manejar_ctrl_c, TRUE);
#else
    signal(SIGINT, manejar_ctrl_c);
    signal(SIGPIPE, SIG_IGN);
#endif

    // 1) IP/puerto broker
    char ip[16], puerto[6];
    printf("Ingrese la direccion IP del broker: \n");
    if (scanf("%15s", ip) != 1) { printf("Entrada invalida.\n"); return 1; }
    printf("Ingrese el puerto del broker: \n");
    if (scanf("%5s", puerto) != 1){ printf("Entrada invalida.\n"); return 1; }
    getchar(); 
    struct sockaddr_in broker;
    memset(&broker, 0, sizeof(broker));
    broker.sin_family = AF_INET;
    broker.sin_port   = htons((unsigned short)atoi(puerto));
#ifdef _WIN32
    InetPton(AF_INET, ip, &broker.sin_addr);
#else
    if (inet_pton(AF_INET, ip, &broker.sin_addr) != 1) {
        printf("IP invalida.\n"); return 1;
    }
#endif

#ifdef _WIN32
    socket_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_udp == INVALID_SOCKET){
        printf("Error al crear el socket UDP.\n");
        WSACleanup();
        return 1;
    }
#else
    socket_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_udp < 0){
        perror("socket");
        return 1;
    }
#endif

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(0); 
#ifdef _WIN32
    if (bind(socket_udp, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR){
        printf("Error al bindear socket local.\n");
        closesocket(socket_udp); WSACleanup();
        return 1;
    }
#else
    if (bind(socket_udp, (struct sockaddr*)&local, sizeof(local)) < 0){
        perror("bind");
        close(socket_udp);
        return 1;
    }
#endif

    // 3) Lanzar hilo receptor
#ifdef _WIN32
    hilo_receptor = CreateThread(NULL, 0, escuchar_mensajes, NULL, 0, NULL);
    if (!hilo_receptor) {
        printf("Error al crear hilo receptor.\n");
        closesocket(socket_udp); WSACleanup();
        return 1;
    }
#else
    if (pthread_create(&hilo_receptor, NULL, escuchar_mensajes, NULL) != 0){
        perror("pthread_create");
        close(socket_udp);
        return 1;
    }
#endif

    // 4) Bucle: pedir tópico (no vacío, sin '|'), enviar SUB, reintentar si broker dice NO_TOPIC
    for (;;) {
        char topico[151];
        do {
            printf("Ingrese el topico al que desea suscribirse: \n> ");
            fflush(stdout);
            if (!fgets(topico, sizeof(topico), stdin)) {
                printf("Fin de entrada.\n");
#ifdef _WIN32
                closesocket(socket_udp); WSACleanup();
#else
                close(socket_udp);
#endif
                return 0;
            }
            chomp(topico);
            if (strchr(topico, '|')) {
                printf("El topico no debe contener '|'. Intente de nuevo.\n");
                topico[0] = '\0';
            }
        } while (topico[0] == '\0');

        if (enviar_sub(socket_udp, &broker, topico) < 0) {
            printf("Error al enviar suscripcion.\n");
            continue;
        }
        printf("[SUB] Enviado: SUB|%s|\n", topico);
        printf("> "); fflush(stdout);

#ifdef _WIN32
        Sleep(800); 
        if (InterlockedCompareExchange(&need_resub, 0, 1) == 1) {
            continue;
        }
#else
        usleep(800 * 1000);
        if (need_resub) { need_resub = 0; continue; }
#endif


    }

#ifdef _WIN32
    closesocket(socket_udp);
    WSACleanup();
#else
    close(socket_udp);
#endif
    return 0;
}
