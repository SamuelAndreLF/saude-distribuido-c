/* Parte comum ao estado, ao município e à federação de teste. */
#ifndef COMUM_H
#define COMUM_H

#include <stddef.h>
#include <stdint.h>

#define MAX_MUN 64
#define MAX_LINHA 512
#define N_TIPOS 2          /* 0 = consulta, 1 = vacina: as duas seguem o mesmo fluxo */
#define N_UF 27            /* UFs numeradas de 0 a 26 (cabe em 5 bits) */
#define MAX_MUNICIPIO 8191 /* cabe em 13 bits */
#define MAX_QUANTIDADE 10  /* máximo por atendimento */

/* operações do município (entram no hash, para um evento não se passar por outro) */
enum { OP_CADASTRO = 1, OP_ATENDIMENTO = 2, OP_FALTA = 3, OP_ESTOQUE = 4 };

/* códigos de resposta do estado */
enum {
    R_OK = 0,
    R_OK_OUTRO_MUNICIPIO = 1,
    R_OK_OUTRO_ESTADO = 2,
    R_HASH_INVALIDO = 10,
    R_DADOS_INVALIDOS = 11,
    R_SEM_ESTOQUE = 12,
    R_NAO_CADASTRADO = 13,
    R_JA_CADASTRADO = 14,
    R_ERRO_INTERNO = 15
};

typedef unsigned __int128 u128; /* o identificador de 82 bits */

typedef struct {
    char ip_federacao[64];
    int porta_federacao;
    int uf;
    char ip_estado[64];
    int porta_estado;
    int pedido_inicial[N_TIPOS]; /* quanto o estado pede à federação quando começa */
    int lote_reposicao;          /* quanto pede quando acaba em um município */
    int n_mun;
    char mun[MAX_MUN + 1][64];   /* nomes, começando no 1 */
    char pasta[128];
} Config;

extern const char *NOME_TIPO[N_TIPOS];
extern const char LETRA_TIPO[N_TIPOS];
extern const char *SIGLA_UF[N_UF];

/* identificador de 82 bits */
u128 id_montar(uint64_t cpf, unsigned uf, unsigned municipio);
uint64_t id_cpf(u128 id);
unsigned id_uf(u128 id);
unsigned id_municipio(u128 id);
void id_para_partes(u128 id, uint64_t *alto, uint64_t *baixo);
int id_de_partes(uint64_t alto, uint64_t baixo, u128 *id);
void id_em_hex(u128 id, char saida[22]);

/* hash SHA-256 (OpenSSL) */
void sha256_hex(const void *dados, size_t n, char saida[65]);
void hash_evento(int operacao, int municipio, uint64_t alto, uint64_t baixo, int tipo, int quantidade,
                 const char *data, const char *nome, char saida[65]);

/* validação */
int cpf_valido(uint64_t cpf);
int texto_para_cpf(const char *txt, uint64_t *cpf);
void cpf_formatado(uint64_t cpf, char saida[32]);
int data_valida(const char *iso);
int texto_para_data(const char *txt, char iso[11]);
void data_de_hoje(char iso[11]);
int nome_valido(const char *nome);
int ler_u64(const char *txt, uint64_t *valor);
int ler_int(const char *txt, int minimo, int maximo, int *valor);
int tipo_da_letra(const char *txt);

/* utilidades */
int ler_config(const char *caminho, Config *cfg);
void aparar(char *txt);
int separar(char *linha, char *campos[], int max);
void agora(char *buf, size_t n);

/* sockets TCP (estado <-> federação) */
int abrir_servidor(int porta);
int conectar(const char *host, int porta);
int ler_linha(int fd, char *buf, size_t n);
int enviar_linha(int fd, const char *formato, ...);

#endif
