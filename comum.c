#include "comum.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

const char *NOME_TIPO[N_TIPOS] = {"consulta", "vacina"};
const char LETRA_TIPO[N_TIPOS] = {'C', 'V'};

/* UFs em ordem alfabética do nome do estado: 0 = Acre ... 8 = Goiás ... 26 = Tocantins.
   O código do IBGE vai até 53 e não cabe em 5 bits, por isso esta numeração. */
const char *SIGLA_UF[N_UF] = {"AC", "AL", "AP", "AM", "BA", "CE", "DF", "ES", "GO", "MA", "MT", "MS", "MG", "PA",
                              "PB", "PR", "PE", "PI", "RJ", "RN", "RS", "RO", "RR", "SC", "SP", "SE", "TO"};

/* ------------------------------------------------ identificador de 82 bits */

/* Bits, do mais alto para o mais baixo: [usuário: 64][estado: 5][município: 13]. */
u128 id_montar(uint64_t cpf, unsigned uf, unsigned municipio)
{
    return ((u128)cpf << 18) | ((u128)(uf & 31) << 13) | (municipio & 8191);
}

uint64_t id_cpf(u128 id) { return (uint64_t)(id >> 18); }
unsigned id_uf(u128 id) { return (unsigned)((id >> 13) & 31); }
unsigned id_municipio(u128 id) { return (unsigned)(id & 8191); }

void id_para_partes(u128 id, uint64_t *alto, uint64_t *baixo)
{
    *alto = (uint64_t)(id >> 64);
    *baixo = (uint64_t)id;
}

/* Devolve 0 se as partes passam de 82 bits. */
int id_de_partes(uint64_t alto, uint64_t baixo, u128 *id)
{
    if (alto >> 18)
        return 0;
    *id = ((u128)alto << 64) | baixo;
    return 1;
}

void id_em_hex(u128 id, char saida[22])
{
    snprintf(saida, 22, "%05llx%016llx", (unsigned long long)(id >> 64), (unsigned long long)id);
}

/* ------------------------------------------------------------------ hash */

void sha256_hex(const void *dados, size_t n, char saida[65])
{
    unsigned char md[SHA256_DIGEST_LENGTH];
    int i;
    SHA256(dados, n, md);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(saida + 2 * i, "%02x", md[i]);
}

/* Junta todos os campos do evento num texto e tira o SHA-256.
   O município calcula antes de mandar; o estado calcula de novo e compara. */
void hash_evento(int operacao, int municipio, uint64_t alto, uint64_t baixo, int tipo, int quantidade,
                 const char *data, const char *nome, char saida[65])
{
    char texto[400];
    int n = snprintf(texto, sizeof texto, "%d|%d|%llu|%llu|%d|%d|%s|%s", operacao, municipio,
                     (unsigned long long)alto, (unsigned long long)baixo, tipo, quantidade, data ? data : "",
                     nome ? nome : "");
    if (n < 0 || n >= (int)sizeof texto)
        n = (int)sizeof texto - 1;
    sha256_hex(texto, (size_t)n, saida);
}

/* ------------------------------------------------------------- validação */

/* CPF: 11 dígitos, não pode ser tudo igual, e os dois dígitos verificadores têm que bater. */
int cpf_valido(uint64_t cpf)
{
    int d[11], i, soma, iguais = 1;
    if (cpf == 0 || cpf > 99999999999ULL)
        return 0;
    for (i = 10; i >= 0; i--) {
        d[i] = (int)(cpf % 10);
        cpf /= 10;
    }
    for (i = 1; i < 11; i++)
        if (d[i] != d[0])
            iguais = 0;
    if (iguais)
        return 0;
    for (soma = 0, i = 0; i < 9; i++)
        soma += d[i] * (10 - i);
    if ((soma * 10) % 11 % 10 != d[9])
        return 0;
    for (soma = 0, i = 0; i < 10; i++)
        soma += d[i] * (11 - i);
    return (soma * 10) % 11 % 10 == d[10];
}

/* Aceita 52998224725 ou 529.982.247-25. */
int texto_para_cpf(const char *txt, uint64_t *cpf)
{
    int digitos = 0;
    uint64_t v = 0;
    for (; *txt; txt++) {
        if (isdigit((unsigned char)*txt)) {
            if (++digitos > 11)
                return 0;
            v = v * 10 + (uint64_t)(*txt - '0');
        } else if (*txt != '.' && *txt != '-') {
            return 0;
        }
    }
    *cpf = v;
    return digitos == 11 && cpf_valido(v);
}

void cpf_formatado(uint64_t cpf, char saida[32])
{
    snprintf(saida, 32, "%03llu.%03llu.%03llu-%02llu", (unsigned long long)(cpf / 100000000ULL),
             (unsigned long long)(cpf / 100000ULL % 1000), (unsigned long long)(cpf / 100 % 1000),
             (unsigned long long)(cpf % 100));
}

void data_de_hoje(char iso[11])
{
    time_t t = time(NULL);
    strftime(iso, 11, "%Y-%m-%d", localtime(&t));
}

/* Data no formato AAAA-MM-DD, que exista no calendário e não esteja no futuro. */
int data_valida(const char *iso)
{
    static const int dias[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int a, m, d, i, limite;
    char hoje[11];
    if (strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-')
        return 0;
    for (i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)iso[i]))
            return 0;
    a = atoi(iso);
    m = atoi(iso + 5);
    d = atoi(iso + 8);
    if (a < 1900 || m < 1 || m > 12 || d < 1)
        return 0;
    limite = dias[m - 1];
    if (m == 2 && ((a % 4 == 0 && a % 100 != 0) || a % 400 == 0))
        limite = 29;
    if (d > limite)
        return 0;
    data_de_hoje(hoje);
    return strcmp(iso, hoje) <= 0;
}

/* O que a pessoa digita: DD/MM/AAAA. Vira AAAA-MM-DD. */
int texto_para_data(const char *txt, char iso[11])
{
    int i;
    if (strlen(txt) != 10 || txt[2] != '/' || txt[5] != '/')
        return 0;
    for (i = 0; i < 10; i++)
        if (i != 2 && i != 5 && !isdigit((unsigned char)txt[i]))
            return 0;
    snprintf(iso, 11, "%.4s-%.2s-%.2s", txt + 6, txt + 3, txt);
    return data_valida(iso);
}

/* Nome: de 3 a 60 caracteres, só letras (com acento também) e espaços. */
int nome_valido(const char *nome)
{
    size_t i, n = strlen(nome);
    int letras = 0;
    if (n < 3 || n > 60)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)nome[i];
        if (isalpha(ch) || ch >= 0x80)
            letras++;
        else if (ch != ' ')
            return 0;
    }
    return letras >= 3;
}

int ler_u64(const char *txt, uint64_t *valor)
{
    size_t i, n = strlen(txt);
    char *fim;
    if (n == 0 || n > 20)
        return 0;
    for (i = 0; i < n; i++)
        if (!isdigit((unsigned char)txt[i]))
            return 0;
    errno = 0;
    *valor = strtoull(txt, &fim, 10);
    return errno == 0 && *fim == 0;
}

int ler_int(const char *txt, int minimo, int maximo, int *valor)
{
    uint64_t v;
    if (!ler_u64(txt, &v) || v < (uint64_t)minimo || v > (uint64_t)maximo)
        return 0;
    *valor = (int)v;
    return 1;
}

int tipo_da_letra(const char *txt)
{
    int t;
    for (t = 0; t < N_TIPOS; t++)
        if (toupper((unsigned char)txt[0]) == LETRA_TIPO[t] && txt[1] == 0)
            return t;
    return -1;
}

/* ------------------------------------------------------------ utilidades */

void aparar(char *txt)
{
    size_t n = strlen(txt), i = 0;
    while (n > 0 && isspace((unsigned char)txt[n - 1]))
        txt[--n] = 0;
    while (txt[i] && isspace((unsigned char)txt[i]))
        i++;
    if (i)
        memmove(txt, txt + i, n - i + 1);
}

int separar(char *linha, char *campos[], int max)
{
    int n = 0;
    char *p = linha;
    while (n < max) {
        campos[n++] = p;
        p = strchr(p, '|');
        if (!p)
            break;
        *p++ = 0;
    }
    return n;
}

void agora(char *buf, size_t n)
{
    time_t t = time(NULL);
    strftime(buf, n, "%d/%m/%Y %H:%M:%S", localtime(&t));
}

int ler_config(const char *caminho, Config *cfg)
{
    char linha[256];
    const char *pasta;
    FILE *f = fopen(caminho, "r");
    if (!f) {
        fprintf(stderr, "Não achei o arquivo de configuração %s\n", caminho);
        return 0;
    }
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->ip_federacao, sizeof cfg->ip_federacao, "127.0.0.1");
    snprintf(cfg->ip_estado, sizeof cfg->ip_estado, "127.0.0.1");
    snprintf(cfg->pasta, sizeof cfg->pasta, "dados");
    cfg->uf = -1;
    cfg->pedido_inicial[0] = cfg->pedido_inicial[1] = 500;
    cfg->lote_reposicao = 10;
    while (fgets(linha, sizeof linha, f)) {
        char *igual, *valor;
        aparar(linha);
        if (linha[0] == '#' || linha[0] == 0 || !(igual = strchr(linha, '=')))
            continue;
        *igual = 0;
        valor = igual + 1;
        aparar(linha);
        aparar(valor);
        if (!strcmp(linha, "ip_federacao")) snprintf(cfg->ip_federacao, sizeof cfg->ip_federacao, "%s", valor);
        else if (!strcmp(linha, "porta_federacao")) cfg->porta_federacao = atoi(valor);
        else if (!strcmp(linha, "uf")) cfg->uf = atoi(valor);
        else if (!strcmp(linha, "ip_estado")) snprintf(cfg->ip_estado, sizeof cfg->ip_estado, "%s", valor);
        else if (!strcmp(linha, "porta_estado")) cfg->porta_estado = atoi(valor);
        else if (!strcmp(linha, "pedido_inicial_consulta")) cfg->pedido_inicial[0] = atoi(valor);
        else if (!strcmp(linha, "pedido_inicial_vacina")) cfg->pedido_inicial[1] = atoi(valor);
        else if (!strcmp(linha, "lote_reposicao")) cfg->lote_reposicao = atoi(valor);
        else if (!strcmp(linha, "pasta")) snprintf(cfg->pasta, sizeof cfg->pasta, "%s", valor);
        else if (!strcmp(linha, "municipio") && cfg->n_mun < MAX_MUN) {
            cfg->n_mun++;
            snprintf(cfg->mun[cfg->n_mun], sizeof cfg->mun[0], "%s", valor);
        }
    }
    fclose(f);
    pasta = getenv("SAUDE_PASTA");
    if (pasta && pasta[0])
        snprintf(cfg->pasta, sizeof cfg->pasta, "%s", pasta);
    if (cfg->uf < 0 || cfg->uf >= N_UF || cfg->n_mun < 1 || cfg->porta_estado <= 0 || cfg->porta_federacao <= 0 ||
        cfg->pedido_inicial[0] < 0 || cfg->pedido_inicial[1] < 0 || cfg->lote_reposicao < 1) {
        fprintf(stderr, "Configuração inválida em %s (uf de 0 a 26, portas e municípios).\n", caminho);
        return 0;
    }
    mkdir(cfg->pasta, 0755);
    return 1;
}

/* ------------------------------------------------------------ sockets TCP */

static void limite_de_tempo(int fd, int segundos)
{
    struct timeval t;
    t.tv_sec = segundos;
    t.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
}

int abrir_servidor(int porta)
{
    struct sockaddr_in end;
    int um = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &um, sizeof um);
    memset(&end, 0, sizeof end);
    end.sin_family = AF_INET;
    end.sin_addr.s_addr = htonl(INADDR_ANY);
    end.sin_port = htons((uint16_t)porta);
    if (bind(fd, (struct sockaddr *)&end, sizeof end) < 0 || listen(fd, 32) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int conectar(const char *host, int porta)
{
    struct addrinfo dica, *lista, *a;
    char txt_porta[16];
    int fd = -1;
    memset(&dica, 0, sizeof dica);
    dica.ai_family = AF_INET;
    dica.ai_socktype = SOCK_STREAM;
    snprintf(txt_porta, sizeof txt_porta, "%d", porta);
    if (getaddrinfo(host, txt_porta, &dica, &lista) != 0)
        return -1;
    for (a = lista; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        limite_de_tempo(fd, 5);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(lista);
    return fd;
}

/* Lê até o fim da linha. Devolve o tamanho, ou -1 se a conexão caiu. */
int ler_linha(int fd, char *buf, size_t n)
{
    size_t i = 0;
    char ch;
    for (;;) {
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0)
            return -1;
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;
        if (i + 1 >= n)
            return -1;
        buf[i++] = ch;
    }
    buf[i] = 0;
    return (int)i;
}

int enviar_linha(int fd, const char *formato, ...)
{
    char buf[MAX_LINHA + 2];
    va_list args;
    int n, enviado = 0;
    va_start(args, formato);
    n = vsnprintf(buf, MAX_LINHA, formato, args);
    va_end(args);
    if (n < 0 || n >= MAX_LINHA)
        return 0;
    buf[n++] = '\n';
    while (enviado < n) {
        ssize_t r = send(fd, buf + enviado, (size_t)(n - enviado), MSG_NOSIGNAL);
        if (r <= 0)
            return 0;
        enviado += (int)r;
    }
    return 1;
}
