#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK(s) closesocket(s)
  #define SOCKERR() WSAGetLastError()
  #define IS_VALID(s) ((s) != INVALID_SOCKET)
  #define SHUT_RDWR SD_BOTH
  sock_t socket_tcp_listen;
  BOOL WINAPI manejar_ctrl_c(DWORD ev){
      if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT){
          printf("\nCerrando el broker...\n");
          if (IS_VALID(socket_tcp_listen)) { shutdown(socket_tcp_listen, SD_BOTH); CLOSESOCK(socket_tcp_listen); }
          WSACleanup();
          exit(0);
      }
      return FALSE;
  }
#else
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <signal.h>
  #include <errno.h>
  typedef int sock_t;
  #define CLOSESOCK(s) close(s)
  #define SOCKERR() errno
  #define IS_VALID(s) ((s) >= 0)
  sock_t socket_tcp_listen;
  void manejar_ctrl_c(int signo){
      (void)signo;
      printf("\nCerrando el broker...\n");
      if (IS_VALID(socket_tcp_listen)) { shutdown(socket_tcp_listen, SHUT_RDWR); CLOSESOCK(socket_tcp_listen); }
      exit(0);
  }
#endif

// Puerto del broker
#define PUERTO 61626

// --------------------
// Estructuras de datos
// --------------------

// *** CAMBIO TCP ***: Suscriptor ahora guarda SOCKET (para enviar por TCP)
struct Suscriptor{
    struct Suscriptor* siguiente;
    sock_t sock;
    struct sockaddr_in addr; // opcional: para logging
};

// Lista enlazada de tópicos
struct Topico{
    char nombre[50];
    struct Suscriptor* suscriptores;
    struct Topico* siguiente;
};

struct Topico* topicos = NULL;

// *** CAMBIO TCP ***: lista de clientes conectados (para select() y lecturas)
struct Cliente {
    sock_t sock;
    struct sockaddr_in addr;
    struct Cliente* siguiente;
};
static struct Cliente* clientes = NULL;

// -------------------------
// Utilidades TCP (framing)
// -------------------------

static int recv_all_tcp(sock_t s, char* buf, int len){
    int got = 0;
    while (got < len){
        int r = (int)recv(s, buf + got, len - got, 0);
        if (r == 0)  return 0;  // peer cerró
        if (r < 0)   return -1; // error
        got += r;
    }
    return got;
}

static int recv_frame_tcp(sock_t s, char** out_buf, int* out_len){
    uint32_t be_len = 0;
    int r = recv_all_tcp(s, (char*)&be_len, 4);
    if (r <= 0) return r; // 0=cierre; -1=error
    uint32_t len = ntohl(be_len);
    if (len == 0 || len > (64u * 1024u)) return -1; // límite defensivo (64 KiB)

    char* p = (char*)malloc(len + 1);
    if (!p) return -1;

    r = recv_all_tcp(s, p, (int)len);
    if (r <= 0){ free(p); return r; }
    p[len] = '\0';
    *out_buf = p; *out_len = (int)len;
    return 1;
}

static int send_all_tcp(sock_t s, const char* buf, int len){
    if (!buf || len < 0) return -1;
    int sent = 0;
    while (sent < len){
#ifdef _WIN32
        int r = (int)send(s, buf + sent, len - sent, 0);
        if (r == SOCKET_ERROR) return -1;
#else
        int r = (int)send(s, buf + sent, len - sent, 0);
        if (r < 0) return -1;
#endif
        if (r == 0) return -1;
        sent += r;
    }
    return sent;
}

static int send_frame_tcp(sock_t s, const char* payload, int len){
    if (!payload || len < 0) return -1;
    if (len > 0x7FFFFFFF) return -1;

    uint32_t be = htonl((uint32_t)len);
    if (send_all_tcp(s, (const char*)&be, 4) < 0) return -1;
    if (send_all_tcp(s, payload, len) < 0) return -1;
    return 4 + len;
}

// -------------------------
// Manejo de tópicos (igual)
// -------------------------

static struct Topico* crear_topico(char* nombre){
    struct Topico* nuevo_topico = (struct Topico*)malloc(sizeof(struct Topico));
    if (!nuevo_topico){
        printf("Error al asignar memoria para el nuevo tópico.\n");
        return NULL;
    }
    strncpy(nuevo_topico->nombre, nombre, sizeof(nuevo_topico->nombre)-1);
    nuevo_topico->nombre[sizeof(nuevo_topico->nombre)-1] = '\0';
    nuevo_topico->suscriptores = NULL;
    nuevo_topico->siguiente = NULL;

    if (topicos == NULL) {
        topicos = nuevo_topico;
    } else {
        struct Topico* actual = topicos;
        while (actual->siguiente != NULL) actual = actual->siguiente;
        actual->siguiente = nuevo_topico;
    }
    printf("Topico %s creado\n", nombre);
    return nuevo_topico;
}

static struct Topico* buscar_topico(char* nombre){
    struct Topico* actual = topicos;
    while (actual){
        if (strcmp(actual->nombre, nombre) == 0) return actual;
        actual = actual->siguiente;
    }
    return NULL;
}

// --------------------------
// Envío de mensajes por TCP
// --------------------------

// *** CAMBIO TCP ***: envia a todos los suscriptores por su socket TCP, usando framing
static void quitar_suscriptor(struct Topico* topico, struct Suscriptor* prev, struct Suscriptor* cur){
    // unlink
    if (prev) prev->siguiente = cur->siguiente;
    else topico->suscriptores = cur->siguiente;
    // cerrar socket
#ifdef _WIN32
    shutdown(cur->sock, SD_BOTH);
#else
    shutdown(cur->sock, SHUT_RDWR);
#endif
    CLOSESOCK(cur->sock);
    free(cur);
}

static int enviar_mensaje(struct Topico* topico, const char* payload){
    if (!topico) return -1;
    int enviados = 0;
    struct Suscriptor* prev = NULL;
    struct Suscriptor* actual = topico->suscriptores;
    size_t len = strlen(payload);

    while (actual){
        if (send_frame_tcp(actual->sock, payload, (int)len) < 0){
            printf("[WARN] error enviando a un suscriptor; se eliminará\n");
            struct Suscriptor* to_free = actual;
            actual = actual->siguiente;
            quitar_suscriptor(topico, prev, to_free);
            continue;
        }
        enviados++;
        prev = actual;
        actual = actual->siguiente;
    }
    return enviados;
}

// Construye "topico/contenido" y lo envía
static int enviar_contenido(char* topico, char* contenido){
    struct Topico* t = buscar_topico(topico);
    if (t == NULL){
        // si no existe, créalo (mantenemos tu comportamiento de creación en PUB implícito)
        t = crear_topico(topico);
        if (!t) return -1;
    }
    // Construir "topico/contenido"
    size_t lt = strlen(topico), lc = strlen(contenido);
    size_t L = lt + 1 + lc; // '/' entre medio
    char* out = (char*)malloc(L + 1);
    if (!out) return -1;
    memcpy(out, topico, lt);
    out[lt] = '/';
    memcpy(out + lt + 1, contenido, lc);
    out[L] = '\0';

    int n = enviar_mensaje(t, out);
    free(out);
    return n;
}

// -------------------------------
// Manejo de suscriptores (TCP)
// -------------------------------

static struct Topico* insertar_suscriptor(struct Topico* topico, sock_t sock, struct sockaddr_in* addr){
    if (!topico || !IS_VALID(sock)){
        printf("Errror: topico o socket nulo.\n");
        return NULL;
    }
    struct Suscriptor* s = (struct Suscriptor*)malloc(sizeof(struct Suscriptor));
    if (!s) return NULL;
    s->sock = sock;
    if (addr) s->addr = *addr; else memset(&s->addr, 0, sizeof(s->addr));
    s->siguiente = NULL;

    if (topico->suscriptores == NULL) topico->suscriptores = s;
    else {
        struct Suscriptor* a = topico->suscriptores;
        while (a->siguiente) a = a->siguiente;
        a->siguiente = s;
    }
    char ipbuf[64];
    inet_ntop(AF_INET, &s->addr.sin_addr, ipbuf, sizeof(ipbuf));
    printf("Usuario %s:%u suscrito a topico %s\n", ipbuf, (unsigned)ntohs(s->addr.sin_port), topico->nombre);
    return topico;
}

static struct Topico* suscribir(char* topico, sock_t sock, struct sockaddr_in* addr){
    struct Topico* t = buscar_topico(topico);
    if (t == NULL){
        printf("El topico %s no existe; se creara\n", topico);
        t = crear_topico(topico);
        if (!t) return NULL;
    }
    return insertar_suscriptor(t, sock, addr);
}

// ------------------------------
// Procesamiento de mensajes
// ------------------------------

static int descomponer_mensaje(char* mensaje,char** instruccion, char** topico, char** contenido){
    *instruccion = (char*)malloc(151);
    *topico      = (char*)malloc(151);
    *contenido   = (char*)malloc(151);
    if (!*instruccion || !*topico || !*contenido){
        printf("Error al asignar memoria.\n");
        return 1;
    }
    int len = (int)strlen(mensaje);
    int i = 0, j = 0, k = 0;
    while(i < len && mensaje[i] != '|'){ (*instruccion)[i] = mensaje[i]; i++; }
    if (i == len || mensaje[i] != '|'){ printf("Error: Mensaje sin separador de instruccion.\n"); return 1; }
    (*instruccion)[i] = '\0'; i++;
    while(i < len && mensaje[i] != '|'){ (*topico)[j++] = mensaje[i++]; }
    if (i == len || mensaje[i] != '|'){ printf("Error: Mensaje sin separador de topico.\n"); return 1; }
    (*topico)[j] = '\0'; i++;
    while(i < len){ (*contenido)[k++] = mensaje[i++]; }
    (*contenido)[k] = '\0';
    return 0;
}

// ------------------------------
// Gestión de clientes TCP
// ------------------------------

static void agregar_cliente(sock_t s, struct sockaddr_in* addr){
    struct Cliente* c = (struct Cliente*)malloc(sizeof(struct Cliente));
    if (!c) { CLOSESOCK(s); return; }
    c->sock = s;
    c->addr = *addr;
    c->siguiente = clientes;
    clientes = c;

    char ipbuf[64];
    inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf));
    printf("[CONN] %s:%u conectado\n", ipbuf, (unsigned)ntohs(addr->sin_port));
}

static void quitar_cliente(sock_t s){
    struct Cliente* prev = NULL;
    struct Cliente* cur = clientes;
    while (cur){
        if (cur->sock == s){
            if (prev) prev->siguiente = cur->siguiente; else clientes = cur->siguiente;
#ifdef _WIN32
            shutdown(s, SD_BOTH);
#else
            shutdown(s, SHUT_RDWR);
#endif
            CLOSESOCK(s);
            free(cur);
            return;
        }
        prev = cur; cur = cur->siguiente;
    }
}

// ------------------------
// Función principal (TCP)
// ------------------------
int main(){
    // Dirección local para escuchar
    struct sockaddr_in broker;
    memset(&broker, 0, sizeof(broker));
    broker.sin_family = AF_INET;
    broker.sin_port   = htons(PUERTO);
    broker.sin_addr.s_addr = htonl(INADDR_ANY);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0){
        printf("Error al iniciar Winsock.\n");
        return 1;
    }
    SetConsoleCtrlHandler(manejar_ctrl_c, TRUE);
#else
    signal(SIGINT, manejar_ctrl_c);
    signal(SIGPIPE, SIG_IGN); // no morir por enviar a socket roto
#endif

    // *** CAMBIO TCP ***: socket de escucha TCP (no UDP)
#ifdef _WIN32
    socket_tcp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!IS_VALID(socket_tcp_listen)){ printf("Error al crear socket: %d\n", SOCKERR()); WSACleanup(); return 1; }
#else
    socket_tcp_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (!IS_VALID(socket_tcp_listen)){ printf("Error al crear socket\n"); return 1; }
#endif

    int yes = 1;
    setsockopt(socket_tcp_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (bind(socket_tcp_listen, (struct sockaddr*)&broker, sizeof(broker)) < 0){
        printf("Error en bind: %d\n", SOCKERR());
#ifdef _WIN32
        CLOSESOCK(socket_tcp_listen); WSACleanup();
#else
        CLOSESOCK(socket_tcp_listen);
#endif
        return 1;
    }
    if (listen(socket_tcp_listen, SOMAXCONN) < 0){
        printf("Error en listen: %d\n", SOCKERR());
#ifdef _WIN32
        CLOSESOCK(socket_tcp_listen); WSACleanup();
#else
        CLOSESOCK(socket_tcp_listen);
#endif
        return 1;
    }

    printf("Broker TCP funcionando en el puerto %d\n", PUERTO);

    // *** CAMBIO TCP ***: bucle principal con select() para múltiples clientes
    for (;;){
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_tcp_listen, &readfds);
        sock_t maxfd = socket_tcp_listen;

        // Añadir todos los clientes
        for (struct Cliente* c = clientes; c; c = c->siguiente){
            FD_SET(c->sock, &readfds);
#ifndef _WIN32
            if (c->sock > maxfd) maxfd = c->sock;
#endif
        }

        int ready;
#ifdef _WIN32
        ready = select(0, &readfds, NULL, NULL, NULL);
#else
        ready = select((int)maxfd + 1, &readfds, NULL, NULL, NULL);
#endif
        if (ready < 0){
            printf("select() error: %d\n", SOCKERR());
            continue;
        }

        // Nueva conexión
        if (FD_ISSET(socket_tcp_listen, &readfds)){
            struct sockaddr_in cliaddr; socklen_t alen = sizeof(cliaddr);
#ifdef _WIN32
            SOCKET cli = accept(socket_tcp_listen, (struct sockaddr*)&cliaddr, &alen);
#else
            int cli = accept(socket_tcp_listen, (struct sockaddr*)&cliaddr, &alen);
#endif
            if (IS_VALID(cli)) agregar_cliente(cli, &cliaddr);
        }

        // Leer de clientes existentes
        struct Cliente* c = clientes;
        while (c){
            struct Cliente* next = c->siguiente; // por si lo quitamos
            if (FD_ISSET(c->sock, &readfds)){
                char* msg = NULL; int mlen = 0;
                int rc = recv_frame_tcp(c->sock, &msg, &mlen);
                if (rc <= 0){
                    if (rc == 0) printf("[DISC] cliente cerro la conexion\n");
                    else         printf("[ERR ] recv_frame_tcp fallo\n");
                    quitar_cliente(c->sock);
                } else {
                    // Procesar "PUB|topico|contenido" o "SUB|topico|..."
                    char *instr=NULL, *top=NULL, *cont=NULL;
                    if (descomponer_mensaje(msg, &instr, &top, &cont) == 0){
                        if (strcmp(instr, "PUB") == 0){
                            enviar_contenido(top, cont);
                        } else if (strcmp(instr, "SUB") == 0){
                            suscribir(top, c->sock, &c->addr);
                        } else {
                            printf("[WARN] instruccion desconocida: %s\n", instr);
                        }
                    }
                    free(instr); free(top); free(cont);
                    free(msg);
                }
            }
            c = next;
        }
    }

    // Nunca llega acá normalmente
#ifdef _WIN32
    CLOSESOCK(socket_tcp_listen);
    WSACleanup();
#else
    CLOSESOCK(socket_tcp_listen);
#endif
    return 0;
}
