/* FEDERAÇÃO FALSA, só para teste.
 *
 * A federação de verdade roda no PC do professor. Este programa imita o papel
 * dela para o grupo conseguir testar sozinho: entrega o que o estado pede e faz
 * o remanejamento entre estados.
 *
 * Socket TCP, uma linha de texto por mensagem, campos separados por "|".
 *
 * O estado manda:
 *   OLA|uf|n_municipios
 *   PEDIDO|uf|municipio|tipo|quantidade        (municipio 0 = para o estado todo)
 *   REMANEJA|uf|municipio|uf_origem|municipio_origem|tipo|quantidade
 * A federação manda:
 *   ENVIO|tipo|quantidade|municipio
 *   DEBITO|municipio|tipo|quantidade           (para o estado de origem)
 * tipo: C = consulta, V = vacina.
 *
 * Uso: ./federacao_falsa [config.conf]
 */
#include "comum.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTES 64

static struct pollfd p[MAX_CLIENTES + 1];
static int uf_do_cliente[MAX_CLIENTES + 1];
static int n = 1; /* p[0] é o socket que aceita conexões */

/* débitos para estados que não estão conectados agora: ficam guardados até o estado aparecer */
static char guardados[256][MAX_LINHA];
static int uf_do_guardado[256], n_guardados;

static int conexao_da_uf(int uf)
{
    int i;
    for (i = 1; i < n; i++)
        if (uf_do_cliente[i] == uf)
            return p[i].fd;
    return -1;
}

static void tratar(int i, char *linha)
{
    char *f[8], msg[MAX_LINHA];
    int campos = separar(linha, f, 8), fd = p[i].fd, uf, mun, uf_o, mun_o, tipo, qtd, k, destino;

    if (campos == 3 && !strcmp(f[0], "OLA") && ler_int(f[1], 0, N_UF - 1, &uf)) {
        uf_do_cliente[i] = uf;
        printf("[federação] estado %s (UF %d) conectou, %s municípios\n", SIGLA_UF[uf], uf, f[2]);
        for (k = 0; k < n_guardados;) { /* entrega o que estava esperando por ele */
            if (uf_do_guardado[k] != uf) {
                k++;
                continue;
            }
            enviar_linha(fd, "%s", guardados[k]);
            n_guardados--;
            uf_do_guardado[k] = uf_do_guardado[n_guardados];
            memcpy(guardados[k], guardados[n_guardados], MAX_LINHA);
        }
    } else if (campos == 5 && !strcmp(f[0], "PEDIDO") && ler_int(f[1], 0, N_UF - 1, &uf) &&
               ler_int(f[2], 0, MAX_MUNICIPIO, &mun) && (tipo = tipo_da_letra(f[3])) >= 0 &&
               ler_int(f[4], 1, 100000000, &qtd)) {
        printf("[federação] %s pediu %d %s(s)%s: enviado\n", SIGLA_UF[uf], qtd, NOME_TIPO[tipo],
               mun ? " de reposição para um município" : " para o estado todo");
        enviar_linha(fd, "ENVIO|%c|%d|%d", LETRA_TIPO[tipo], qtd, mun);
    } else if (campos == 7 && !strcmp(f[0], "REMANEJA") && ler_int(f[1], 0, N_UF - 1, &uf) &&
               ler_int(f[2], 1, MAX_MUNICIPIO, &mun) && ler_int(f[3], 0, N_UF - 1, &uf_o) &&
               ler_int(f[4], 1, MAX_MUNICIPIO, &mun_o) && (tipo = tipo_da_letra(f[5])) >= 0 &&
               ler_int(f[6], 1, 100000000, &qtd)) {
        printf("[federação] remanejamento: %s atendeu morador de %s (município %d), %d %s(s). "
               "%s recebe a reposição, %s recebe a menos\n",
               SIGLA_UF[uf], SIGLA_UF[uf_o], mun_o, qtd, NOME_TIPO[tipo], SIGLA_UF[uf], SIGLA_UF[uf_o]);
        enviar_linha(fd, "ENVIO|%c|%d|%d", LETRA_TIPO[tipo], qtd, mun);
        snprintf(msg, sizeof msg, "DEBITO|%d|%c|%d", mun_o, LETRA_TIPO[tipo], qtd);
        destino = conexao_da_uf(uf_o);
        if (destino >= 0)
            enviar_linha(destino, "%s", msg);
        else if (n_guardados < 256) {
            uf_do_guardado[n_guardados] = uf_o;
            snprintf(guardados[n_guardados++], MAX_LINHA, "%s", msg);
        }
    } else {
        printf("[federação] mensagem que não entendi: %s\n", f[0]);
        enviar_linha(fd, "ERRO|mensagem desconhecida");
    }
}

int main(int argc, char **argv)
{
    Config cfg;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (!ler_config(argc > 1 ? argv[1] : "config.conf", &cfg))
        return 1;
    p[0].fd = abrir_servidor(cfg.porta_federacao);
    p[0].events = POLLIN;
    if (p[0].fd < 0) {
        fprintf(stderr, "Não consegui abrir a porta %d (já tem uma federação rodando?)\n", cfg.porta_federacao);
        return 1;
    }
    printf("[federação] FALSA, para teste, no ar na porta %d\n", cfg.porta_federacao);
    for (;;) {
        if (poll(p, (nfds_t)n, -1) < 0)
            continue;
        if (p[0].revents & POLLIN) {
            int fd = accept(p[0].fd, NULL, NULL);
            if (fd >= 0 && n <= MAX_CLIENTES) {
                p[n].fd = fd;
                p[n].events = POLLIN;
                p[n].revents = 0;
                uf_do_cliente[n++] = -1;
            } else if (fd >= 0) {
                close(fd);
            }
        }
        for (i = 1; i < n; i++) {
            char linha[MAX_LINHA];
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            if (ler_linha(p[i].fd, linha, sizeof linha) < 0) { /* o estado desconectou */
                if (uf_do_cliente[i] >= 0)
                    printf("[federação] estado %s desconectou\n", SIGLA_UF[uf_do_cliente[i]]);
                close(p[i].fd);
                n--;
                p[i] = p[n];
                uf_do_cliente[i] = uf_do_cliente[n];
                i--;
                continue;
            }
            tratar(i, linha);
        }
    }
}
