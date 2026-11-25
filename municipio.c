/* MUNICÍPIO: terminal de atendimento.
 *
 * - Cliente RPC do estado (funções do saude.x).
 * - Valida tudo o que é digitado antes de mandar.
 * - Todo evento vai com o hash SHA-256 dos dados.
 * - Não tem banco: os atendimentos ficam numa pilha em memória (vetor que cresce).
 *
 * Uso: ./municipio <numero do municipio> [config.conf]
 */
#include "comum.h"
#include "saude.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char quando[32];
    uint64_t cpf;
    unsigned uf, municipio;
    int tipo, quantidade;
    char data[11];
    char situacao[24];
} Registro;

static Config cfg;
static int MUN;
static CLIENT *cliente;
static int interativo;

/* a pilha de registros do município */
static Registro *pilha;
static int topo, capacidade;

static void empilhar(const Registro *r)
{
    if (topo == capacidade) {
        capacidade = capacidade ? capacidade * 2 : 16;
        pilha = realloc(pilha, (size_t)capacidade * sizeof *pilha);
        if (!pilha) {
            fprintf(stderr, "Sem memória\n");
            exit(1);
        }
    }
    pilha[topo++] = *r;
}

/* ------------------------------------------------------------------- RPC */

static int ligar(void)
{
    struct sockaddr_in end;
    struct timeval limite = {5, 0};
    int sock = RPC_ANYSOCK;

    memset(&end, 0, sizeof end);
    end.sin_family = AF_INET;
    end.sin_port = htons((uint16_t)cfg.porta_estado); /* porta fixa: não precisa do rpcbind */
    if (inet_pton(AF_INET, cfg.ip_estado, &end.sin_addr) != 1) {
        printf("ip_estado inválido no config: %s\n", cfg.ip_estado);
        return 0;
    }
    cliente = clnttcp_create(&end, SAUDE_PROG, SAUDE_VERS, &sock, 0, 0);
    if (!cliente) {
        if (getenv("SAUDE_DEPURAR"))
            clnt_pcreateerror("clnttcp_create");
        return 0;
    }
    clnt_control(cliente, CLSET_TIMEOUT, (char *)&limite);
    return 1;
}

/* Calcula o hash do evento, chama o estado e devolve a resposta (NULL se não deu para falar com ele). */
static resposta *chamar(int operacao, evento *e)
{
    static char hash[65];
    resposta *r = NULL;

    e->municipio = MUN;
    if (!e->data) e->data = "";
    if (!e->nome) e->nome = "";
    hash_evento(operacao, e->municipio, e->usuario.alto, e->usuario.baixo, e->tipo, e->quantidade, e->data, e->nome,
                hash);
    e->hash = hash;
    if (!cliente && !ligar()) {
        printf("Sem conexão com o estado (%s:%d). Confira se ele está ligado.\n", cfg.ip_estado, cfg.porta_estado);
        return NULL;
    }
    switch (operacao) {
    case OP_CADASTRO: r = cadastrar_1(e, cliente); break;
    case OP_ATENDIMENTO: r = atender_1(e, cliente); break;
    case OP_FALTA: r = avisar_falta_1(e, cliente); break;
    case OP_ESTOQUE: r = consultar_estoque_1(e, cliente); break;
    }
    if (!r) { /* o estado caiu no meio: na próxima chamada tenta ligar de novo */
        clnt_destroy(cliente);
        cliente = NULL;
        printf("O estado não respondeu (%s:%d).\n", cfg.ip_estado, cfg.porta_estado);
    }
    return r;
}

/* ------------------------------------------------- leitura com validação */

static int ler_entrada(const char *pergunta, char *buf, size_t n)
{
    if (interativo) {
        printf("%s", pergunta);
        fflush(stdout);
    }
    if (!fgets(buf, (int)n, stdin))
        return 0;
    if (!strchr(buf, '\n') && !feof(stdin)) { /* digitou mais do que cabe */
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF)
            ;
        buf[0] = 0;
        printf("Texto grande demais.\n");
    }
    aparar(buf);
    return 1;
}

/* Cada função abaixo fica perguntando até vir um valor válido. Devolve 0 se a entrada acabou. */

static int perguntar_cpf(uint64_t *cpf)
{
    char buf[64];
    for (;;) {
        if (!ler_entrada("CPF (11 dígitos): ", buf, sizeof buf))
            return 0;
        if (texto_para_cpf(buf, cpf))
            return 1;
        printf("CPF inválido: \"%s\". Confira os 11 dígitos.\n", buf);
    }
}

static int perguntar_numero(const char *pergunta, int minimo, int maximo, int tem_padrao, int padrao, int *valor)
{
    char buf[64];
    for (;;) {
        if (!ler_entrada(pergunta, buf, sizeof buf))
            return 0;
        if (buf[0] == 0 && tem_padrao) {
            *valor = padrao;
            return 1;
        }
        if (ler_int(buf, minimo, maximo, valor))
            return 1;
        printf("Valor inválido: \"%s\". Digite um número de %d a %d.\n", buf, minimo, maximo);
    }
}

static int perguntar_data(char iso[11])
{
    char buf[64];
    for (;;) {
        if (!ler_entrada("Data do atendimento, DD/MM/AAAA [Enter = hoje]: ", buf, sizeof buf))
            return 0;
        if (buf[0] == 0) {
            data_de_hoje(iso);
            return 1;
        }
        if (texto_para_data(buf, iso))
            return 1;
        printf("Data inválida: \"%s\". Use DD/MM/AAAA, uma data que exista e que não seja no futuro.\n", buf);
    }
}

/* ------------------------------------------------------------- operações */

static void ver_estoque(void)
{
    evento e;
    resposta *r;
    memset(&e, 0, sizeof e);
    if (!(r = chamar(OP_ESTOQUE, &e)))
        return;
    if (r->codigo != R_OK) {
        printf("Recusado pelo estado: %s\n", r->mensagem);
        return;
    }
    printf("Município %d %s (%s). Estoque: %d consultas, %d vacinas.\n", MUN, cfg.mun[MUN], SIGLA_UF[cfg.uf],
           r->estoque_consulta, r->estoque_vacina);
}

static void cadastrar(void)
{
    char nome[128], cpf_txt[32], hex[22];
    uint64_t cpf;
    u128 id;
    evento e;
    resposta *r;

    if (!perguntar_cpf(&cpf))
        return;
    for (;;) {
        if (!ler_entrada("Nome: ", nome, sizeof nome))
            return;
        if (nome_valido(nome))
            break;
        printf("Nome inválido. Use só letras e espaços, de 3 a 60 caracteres.\n");
    }
    memset(&e, 0, sizeof e);
    id = id_montar(cpf, (unsigned)cfg.uf, (unsigned)MUN);
    id_para_partes(id, &e.usuario.alto, &e.usuario.baixo);
    e.nome = nome;
    if (!(r = chamar(OP_CADASTRO, &e)))
        return;
    cpf_formatado(cpf, cpf_txt);
    id_em_hex(id, hex);
    if (r->codigo == R_OK)
        printf("Cadastrado: %s, CPF %s. Identificador de 82 bits: %s\n", nome, cpf_txt, hex);
    else
        printf("Recusado pelo estado: %s\n", r->mensagem);
}

static void atender(void)
{
    char pergunta[128], cpf_txt[32];
    int escolha, uf, mun_origem;
    uint64_t cpf;
    u128 id;
    evento e;
    resposta *r;
    Registro reg;

    memset(&e, 0, sizeof e);
    if (!perguntar_numero("Tipo: 1 = vacina, 2 = consulta: ", 1, 2, 0, 0, &escolha))
        return;
    e.tipo = escolha == 1 ? 1 : 0;
    if (!perguntar_cpf(&cpf))
        return;
    snprintf(pergunta, sizeof pergunta, "Estado onde mora, de 0 a %d [Enter = %d, %s]: ", N_UF - 1, cfg.uf,
             SIGLA_UF[cfg.uf]);
    if (!perguntar_numero(pergunta, 0, N_UF - 1, 1, cfg.uf, &uf))
        return;
    snprintf(pergunta, sizeof pergunta, "Município onde mora%s: ", uf == cfg.uf ? " [Enter = este]" : "");
    if (!perguntar_numero(pergunta, 1, uf == cfg.uf ? cfg.n_mun : MAX_MUNICIPIO, uf == cfg.uf, MUN, &mun_origem))
        return;
    snprintf(pergunta, sizeof pergunta, "Quantidade, de 1 a %d [Enter = 1]: ", MAX_QUANTIDADE);
    if (!perguntar_numero(pergunta, 1, MAX_QUANTIDADE, 1, 1, &e.quantidade))
        return;
    if (!perguntar_data(reg.data))
        return;
    e.data = reg.data;
    id = id_montar(cpf, (unsigned)uf, (unsigned)mun_origem);
    id_para_partes(id, &e.usuario.alto, &e.usuario.baixo);
    cpf_formatado(cpf, cpf_txt);

    /* Antes de atender, vê com o estado se ainda tem. */
    if (!(r = chamar(OP_ESTOQUE, &e)))
        return;
    if (r->codigo == R_OK && (e.tipo ? r->estoque_vacina : r->estoque_consulta) < e.quantidade) {
        printf("Acabou a %s neste município: não dá para atender.\n", NOME_TIPO[e.tipo]);
        if ((r = chamar(OP_FALTA, &e)) != NULL)
            printf("Estado avisado: %s\n", r->mensagem);
        return;
    }

    if (!(r = chamar(OP_ATENDIMENTO, &e)))
        return;
    if (r->codigo == R_SEM_ESTOQUE) {
        printf("Acabou a %s neste município: não dá para atender. O estado já registrou a falta.\n",
               NOME_TIPO[e.tipo]);
        return;
    }
    if (r->codigo != R_OK && r->codigo != R_OK_OUTRO_MUNICIPIO && r->codigo != R_OK_OUTRO_ESTADO) {
        printf("Recusado pelo estado: %s\n", r->mensagem);
        return;
    }

    /* deu certo: registra aqui no município também */
    agora(reg.quando, sizeof reg.quando);
    reg.cpf = cpf;
    reg.uf = (unsigned)uf;
    reg.municipio = (unsigned)mun_origem;
    reg.tipo = e.tipo;
    reg.quantidade = e.quantidade;
    snprintf(reg.situacao, sizeof reg.situacao, "%s",
             r->codigo == R_OK ? "morador daqui" : r->codigo == R_OK_OUTRO_MUNICIPIO ? "outro municipio" : "outro estado");
    empilhar(&reg);

    printf("%s registrada: %d para o CPF %s", e.tipo ? "Vacina" : "Consulta", e.quantidade, cpf_txt);
    if (r->codigo == R_OK_OUTRO_MUNICIPIO)
        printf(" (mora em outro município; o estado registrou e fez a reposição)");
    else if (r->codigo == R_OK_OUTRO_ESTADO)
        printf(" (mora em %s; o estado pediu o remanejamento à federação)", SIGLA_UF[uf]);
    printf(". Estoque agora: %d consultas, %d vacinas.\n", r->estoque_consulta, r->estoque_vacina);
}

static void ver_registros(void)
{
    char cpf_txt[32];
    int i;
    printf("Registros deste município (%d), do mais novo para o mais antigo:\n", topo);
    for (i = topo - 1; i >= 0; i--) {
        cpf_formatado(pilha[i].cpf, cpf_txt);
        printf("  %s  %-8s x%-2d  CPF %s  mora em %s-%u  data %s  %s\n", pilha[i].quando, NOME_TIPO[pilha[i].tipo],
               pilha[i].quantidade, cpf_txt, SIGLA_UF[pilha[i].uf], pilha[i].municipio, pilha[i].data,
               pilha[i].situacao);
    }
    if (!topo)
        printf("  (nenhum ainda)\n");
}

/* Só para teste: manda um evento com o hash trocado, para mostrar que o estado recusa. */
static void teste_hash_errado(void)
{
    evento e;
    resposta *r;
    memset(&e, 0, sizeof e);
    e.municipio = MUN;
    e.data = "";
    e.nome = "";
    e.hash = "0000000000000000000000000000000000000000000000000000000000000000";
    if (!ligar() || !(r = consultar_estoque_1(&e, cliente))) {
        printf("Sem conexão com o estado.\n");
        return;
    }
    printf("Resposta do estado (código %d): %s\n", r->codigo, r->mensagem);
}

int main(int argc, char **argv)
{
    char opcao[64];
    const char *config = "config.conf";
    int i, hash_errado = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--hash-errado"))
            hash_errado = 1;
        else
            config = argv[i];
    }
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <numero do municipio> [config.conf]\n", argv[0]);
        return 1;
    }
    if (!ler_config(config, &cfg))
        return 1;
    if (!ler_int(argv[1], 1, cfg.n_mun, &MUN)) {
        fprintf(stderr, "Município inválido: use um número de 1 a %d.\n", cfg.n_mun);
        return 1;
    }
    if (hash_errado) {
        teste_hash_errado();
        return 0;
    }
    interativo = isatty(STDIN_FILENO);

    ver_estoque();
    for (;;) {
        if (interativo)
            printf("\n1 Cadastrar morador   2 Atender (vacina ou consulta)   3 Ver estoque   4 Ver registros   0 Sair\n");
        if (!ler_entrada("Opção: ", opcao, sizeof opcao) || !strcmp(opcao, "0"))
            break;
        if (!strcmp(opcao, "1")) cadastrar();
        else if (!strcmp(opcao, "2")) atender();
        else if (!strcmp(opcao, "3")) ver_estoque();
        else if (!strcmp(opcao, "4")) ver_registros();
        else if (opcao[0]) printf("Opção inválida: digite 0, 1, 2, 3 ou 4.\n");
    }
    return 0;
}
