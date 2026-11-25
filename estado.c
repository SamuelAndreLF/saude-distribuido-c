/* ESTADO.
 *
 * - Servidor RPC: atende as chamadas dos municípios (funções do saude.x).
 * - Banco SQLite: usuários, estoque de cada município, atendimentos e ocorrências.
 * - Cliente da federação por socket TCP: pede vacinas/consultas, pede o
 *   remanejamento quando atende gente de outro estado e recebe o que ela manda.
 *
 * São três threads mexendo no mesmo estoque, por isso tudo que toca no banco
 * passa pelo mutex "trava":
 *   1. a principal, que roda o servidor RPC (municípios);
 *   2. a da federação, que mantém o socket aberto;
 *   3. a do teclado, com os comandos do operador.
 *
 * Uso: ./estado [config.conf]
 */
#include "comum.h"
#include "saude.h"

#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void saude_prog_1(struct svc_req *rqstp, SVCXPRT *transp); /* gerado pelo rpcgen em saude_svc.c */

static Config cfg;
static sqlite3 *db;
static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static int pedido_pendente[MAX_MUN + 1][N_TIPOS]; /* já pedi reposição e ainda não chegou */

/* ------------------------------------------------------------------ banco */

static int sql(const char *formato, ...)
{
    va_list args;
    char *comando, *erro = NULL;
    int r;
    va_start(args, formato);
    comando = sqlite3_vmprintf(formato, args);
    va_end(args);
    r = sqlite3_exec(db, comando, NULL, NULL, &erro);
    if (r != SQLITE_OK) {
        fprintf(stderr, "[estado] erro no banco: %s\n", erro ? erro : "?");
        sqlite3_free(erro);
    }
    sqlite3_free(comando);
    return r == SQLITE_OK;
}

/* Roda uma consulta que devolve um número só. Se não vier linha, devolve "padrao". */
static long long sql_numero(long long padrao, const char *formato, ...)
{
    va_list args;
    char *comando;
    sqlite3_stmt *st;
    long long valor = padrao;
    va_start(args, formato);
    comando = sqlite3_vmprintf(formato, args);
    va_end(args);
    if (sqlite3_prepare_v2(db, comando, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            valor = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_free(comando);
    return valor;
}

static void criar_banco(void)
{
    int m, t;
    sql("CREATE TABLE IF NOT EXISTS usuarios ("
        " cpf INTEGER NOT NULL, uf INTEGER NOT NULL, municipio INTEGER NOT NULL, nome TEXT NOT NULL,"
        " PRIMARY KEY (cpf, uf, municipio))"); /* chave única = id + estado + município */
    sql("CREATE TABLE IF NOT EXISTS estoque ("
        " municipio INTEGER NOT NULL, tipo INTEGER NOT NULL, quantidade INTEGER NOT NULL,"
        " PRIMARY KEY (municipio, tipo))");
    sql("CREATE TABLE IF NOT EXISTS atendimentos ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, registrado_em TEXT, municipio INTEGER, cpf INTEGER,"
        " uf_origem INTEGER, municipio_origem INTEGER, tipo INTEGER, quantidade INTEGER, data TEXT)");
    sql("CREATE TABLE IF NOT EXISTS ocorrencias ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, registrado_em TEXT, ocorrencia TEXT, municipio INTEGER,"
        " cpf INTEGER, uf_origem INTEGER, municipio_origem INTEGER, tipo INTEGER, quantidade INTEGER, detalhe TEXT)");
    sql("CREATE TABLE IF NOT EXISTS fila_federacao (id INTEGER PRIMARY KEY AUTOINCREMENT, mensagem TEXT)");
    sql("CREATE TABLE IF NOT EXISTS meta (chave TEXT PRIMARY KEY, valor TEXT)");
    for (m = 1; m <= cfg.n_mun; m++)
        for (t = 0; t < N_TIPOS; t++)
            sql("INSERT OR IGNORE INTO estoque VALUES (%d, %d, 0)", m, t);
}

static int estoque_de(int mun, int tipo)
{
    return (int)sql_numero(0, "SELECT quantidade FROM estoque WHERE municipio=%d AND tipo=%d", mun, tipo);
}

static void mudar_estoque(int mun, int tipo, int diferenca)
{
    sql("UPDATE estoque SET quantidade = MAX(0, quantidade + (%d)) WHERE municipio=%d AND tipo=%d", diferenca, mun,
        tipo);
}

/* Toda ocorrência fica guardada no banco do estado e aparece na tela. */
static void ocorrencia(const char *o_que, int mun, const u128 *id, int tipo, int qtd, const char *detalhe)
{
    char data[32];
    agora(data, sizeof data);
    if (id)
        sql("INSERT INTO ocorrencias (registrado_em, ocorrencia, municipio, cpf, uf_origem, municipio_origem, tipo,"
            " quantidade, detalhe) VALUES (%Q, %Q, %d, %lld, %d, %d, %d, %d, %Q)",
            data, o_que, mun, (long long)id_cpf(*id), (int)id_uf(*id), (int)id_municipio(*id), tipo, qtd, detalhe);
    else
        sql("INSERT INTO ocorrencias (registrado_em, ocorrencia, municipio, tipo, quantidade, detalhe)"
            " VALUES (%Q, %Q, %d, %d, %d, %Q)",
            data, o_que, mun, tipo, qtd, detalhe);
    printf("[estado %s] %s %s | município %d | %s\n", SIGLA_UF[cfg.uf], data, o_que, mun, detalhe);
}

/* Mensagens para a federação entram numa fila no banco. A thread da federação
   envia assim que tiver conexão, então nada se perde se ela estiver fora do ar. */
static void para_federacao(const char *formato, ...)
{
    char msg[MAX_LINHA];
    va_list args;
    va_start(args, formato);
    vsnprintf(msg, sizeof msg, formato, args);
    va_end(args);
    sql("INSERT INTO fila_federacao (mensagem) VALUES (%Q)", msg);
}

static void pedir_reposicao(int mun, int tipo)
{
    if (pedido_pendente[mun][tipo])
        return; /* já pedi, é só esperar chegar */
    pedido_pendente[mun][tipo] = 1;
    para_federacao("PEDIDO|%d|%d|%c|%d", cfg.uf, mun, LETRA_TIPO[tipo], cfg.lote_reposicao);
}

/* Divide igualmente o que chegou entre os municípios (o que sobra da divisão vai para os primeiros). */
static void repartir(int tipo, int quantidade)
{
    int m;
    for (m = 1; m <= cfg.n_mun; m++)
        mudar_estoque(m, tipo, quantidade / cfg.n_mun + (m <= quantidade % cfg.n_mun ? 1 : 0));
}

/* ------------------------------------------- chamadas RPC dos municípios */

static resposta resp;
static char resp_msg[TAM_MSG];

/* Monta a resposta e solta o mutex. Toda função RPC termina aqui. */
static resposta *responder(int codigo, int mun, const char *formato, ...)
{
    va_list args;
    va_start(args, formato);
    vsnprintf(resp_msg, sizeof resp_msg, formato, args);
    va_end(args);
    resp.codigo = codigo;
    resp.mensagem = resp_msg;
    resp.estoque_consulta = mun >= 1 && mun <= cfg.n_mun ? estoque_de(mun, 0) : 0;
    resp.estoque_vacina = mun >= 1 && mun <= cfg.n_mun ? estoque_de(mun, 1) : 0;
    pthread_mutex_unlock(&trava);
    return &resp;
}

/* Recalcula o hash e confere os campos que todo evento tem.
   Devolve 0 se está tudo certo; senão devolve o código de erro e preenche "motivo". */
static int conferir(const evento *e, int operacao, u128 *id, const char **motivo)
{
    char esperado[65];
    int mun = e->municipio >= 1 && e->municipio <= cfg.n_mun ? e->municipio : 0;

    hash_evento(operacao, e->municipio, e->usuario.alto, e->usuario.baixo, e->tipo, e->quantidade, e->data, e->nome,
                esperado);
    if (strcmp(esperado, e->hash) != 0) {
        ocorrencia("HASH_INVALIDO", mun, NULL, -1, 0, "o hash recebido não confere com os dados; evento recusado");
        *motivo = "o hash SHA-256 não confere, evento recusado";
        return R_HASH_INVALIDO;
    }
    if (!mun) {
        *motivo = "esse município não existe neste estado";
        return R_DADOS_INVALIDOS;
    }
    if (operacao == OP_ESTOQUE)
        return 0;
    if (!id_de_partes(e->usuario.alto, e->usuario.baixo, id) || !cpf_valido(id_cpf(*id))) {
        *motivo = "CPF inválido";
        return R_DADOS_INVALIDOS;
    }
    if (id_uf(*id) >= N_UF || id_municipio(*id) < 1 ||
        ((int)id_uf(*id) == cfg.uf && (int)id_municipio(*id) > cfg.n_mun)) {
        *motivo = "estado ou município de origem inválido";
        return R_DADOS_INVALIDOS;
    }
    if (operacao == OP_CADASTRO)
        return 0;
    if (e->tipo < 0 || e->tipo >= N_TIPOS) {
        *motivo = "tipo inválido (0 = consulta, 1 = vacina)";
        return R_DADOS_INVALIDOS;
    }
    if (e->quantidade < 1 || e->quantidade > MAX_QUANTIDADE) {
        *motivo = "quantidade inválida";
        return R_DADOS_INVALIDOS;
    }
    if (!data_valida(e->data)) {
        *motivo = "data inválida";
        return R_DADOS_INVALIDOS;
    }
    if ((int)id_uf(*id) == cfg.uf &&
        !sql_numero(0, "SELECT 1 FROM usuarios WHERE cpf=%lld AND uf=%d AND municipio=%d", (long long)id_cpf(*id),
                    (int)id_uf(*id), (int)id_municipio(*id))) {
        *motivo = "usuário não cadastrado nesse município";
        return R_NAO_CADASTRADO;
    }
    return 0;
}

resposta *cadastrar_1_svc(evento *e, struct svc_req *rq)
{
    u128 id = 0;
    const char *motivo = "";
    int erro;
    (void)rq;
    pthread_mutex_lock(&trava);
    if ((erro = conferir(e, OP_CADASTRO, &id, &motivo)) != 0)
        return responder(erro, e->municipio, "%s", motivo);
    if ((int)id_uf(id) != cfg.uf || (int)id_municipio(id) != e->municipio)
        return responder(R_DADOS_INVALIDOS, e->municipio, "o município só cadastra quem mora nele");
    if (!nome_valido(e->nome))
        return responder(R_DADOS_INVALIDOS, e->municipio, "nome inválido");
    if (sql_numero(0, "SELECT 1 FROM usuarios WHERE cpf=%lld AND uf=%d AND municipio=%d", (long long)id_cpf(id),
                   (int)id_uf(id), (int)id_municipio(id)))
        return responder(R_JA_CADASTRADO, e->municipio, "já existe usuário com esse CPF nesse município e estado");
    if (!sql("INSERT INTO usuarios VALUES (%lld, %d, %d, %Q)", (long long)id_cpf(id), (int)id_uf(id),
             (int)id_municipio(id), e->nome))
        return responder(R_ERRO_INTERNO, e->municipio, "erro ao gravar no banco");
    ocorrencia("CADASTRO", e->municipio, &id, -1, 0, e->nome);
    return responder(R_OK, e->municipio, "cadastrado");
}

resposta *atender_1_svc(evento *e, struct svc_req *rq)
{
    u128 id = 0;
    const char *motivo = "";
    char data[32], detalhe[160];
    int erro, mun = e->municipio, tipo = e->tipo, qtd = e->quantidade;
    (void)rq;
    pthread_mutex_lock(&trava);
    if ((erro = conferir(e, OP_ATENDIMENTO, &id, &motivo)) != 0)
        return responder(erro, mun, "%s", motivo);

    if (estoque_de(mun, tipo) < qtd) { /* acabou: não atende, registra e pede reposição */
        ocorrencia("FALTA_DE_ESTOQUE", mun, &id, tipo, qtd, "sem estoque; reposição pedida à federação");
        pedir_reposicao(mun, tipo);
        return responder(R_SEM_ESTOQUE, mun, "sem estoque de %s neste município", NOME_TIPO[tipo]);
    }

    sql("BEGIN");
    agora(data, sizeof data);
    mudar_estoque(mun, tipo, -qtd);
    sql("INSERT INTO atendimentos (registrado_em, municipio, cpf, uf_origem, municipio_origem, tipo, quantidade, data)"
        " VALUES (%Q, %d, %lld, %d, %d, %d, %d, %Q)",
        data, mun, (long long)id_cpf(id), (int)id_uf(id), (int)id_municipio(id), tipo, qtd, e->data);

    if ((int)id_uf(id) != cfg.uf) {
        /* Pessoa de outro estado: a UF de origem sai dos 5 bits do identificador.
           Pede à federação o remanejamento (ela repõe aqui e desconta do estado de origem). */
        snprintf(detalhe, sizeof detalhe, "%d %s para morador de %s, município %u; remanejamento pedido à federação",
                 qtd, NOME_TIPO[tipo], SIGLA_UF[id_uf(id)], id_municipio(id));
        ocorrencia("ATENDIMENTO_DE_OUTRO_ESTADO", mun, &id, tipo, qtd, detalhe);
        para_federacao("REMANEJA|%d|%d|%d|%d|%c|%d", cfg.uf, mun, (int)id_uf(id), (int)id_municipio(id),
                       LETRA_TIPO[tipo], qtd);
        sql("COMMIT");
        return responder(R_OK_OUTRO_ESTADO, mun, "atendido; morador de outro estado, federação avisada");
    }
    if ((int)id_municipio(id) != mun) {
        /* Pessoa de outro município do nosso estado: o estado repõe a dose para
           quem atendeu, tirando do município onde a pessoa mora. */
        int origem = (int)id_municipio(id), tirar = estoque_de(origem, tipo);
        if (tirar > qtd)
            tirar = qtd;
        mudar_estoque(origem, tipo, -tirar);
        mudar_estoque(mun, tipo, tirar);
        if (tirar < qtd) /* a origem não tinha tudo: o que faltou vem da federação */
            para_federacao("PEDIDO|%d|%d|%c|%d", cfg.uf, mun, LETRA_TIPO[tipo], qtd - tirar);
        snprintf(detalhe, sizeof detalhe, "%d %s para morador de %s; reposto %d vindo de %s", qtd, NOME_TIPO[tipo],
                 cfg.mun[origem], tirar, cfg.mun[origem]);
        ocorrencia("ATENDIMENTO_DE_OUTRO_MUNICIPIO", mun, &id, tipo, qtd, detalhe);
        sql("COMMIT");
        return responder(R_OK_OUTRO_MUNICIPIO, mun, "atendido; morador de outro município, reposição feita");
    }
    snprintf(detalhe, sizeof detalhe, "%d %s", qtd, NOME_TIPO[tipo]);
    ocorrencia("ATENDIMENTO", mun, &id, tipo, qtd, detalhe);
    sql("COMMIT");
    return responder(R_OK, mun, "atendido");
}

resposta *avisar_falta_1_svc(evento *e, struct svc_req *rq)
{
    u128 id = 0;
    const char *motivo = "";
    int erro;
    (void)rq;
    pthread_mutex_lock(&trava);
    if ((erro = conferir(e, OP_FALTA, &id, &motivo)) != 0 && erro != R_NAO_CADASTRADO)
        return responder(erro, e->municipio, "%s", motivo);
    ocorrencia("FALTA_DE_ESTOQUE", e->municipio, &id, e->tipo, e->quantidade,
               "o município avisou que acabou; reposição pedida à federação");
    pedir_reposicao(e->municipio, e->tipo);
    return responder(R_OK, e->municipio, "falta registrada; o estado pediu reposição à federação");
}

resposta *consultar_estoque_1_svc(evento *e, struct svc_req *rq)
{
    u128 id = 0;
    const char *motivo = "";
    int erro;
    (void)rq;
    pthread_mutex_lock(&trava);
    if ((erro = conferir(e, OP_ESTOQUE, &id, &motivo)) != 0)
        return responder(erro, e->municipio, "%s", motivo);
    return responder(R_OK, e->municipio, "%s", cfg.mun[e->municipio]);
}

/* ------------------------------------------------- thread da federação */

/* O que a federação manda:
     ENVIO|tipo|quantidade|municipio    (municipio 0 = dividir entre todos)
     DEBITO|municipio|tipo|quantidade   (morador nosso foi atendido em outro estado) */
static void tratar_federacao(char *linha)
{
    char *f[5], detalhe[160];
    int n = separar(linha, f, 5), tipo, qtd, mun;

    pthread_mutex_lock(&trava);
    if (n == 4 && !strcmp(f[0], "ENVIO") && (tipo = tipo_da_letra(f[1])) >= 0 && ler_int(f[2], 0, 100000000, &qtd) &&
        ler_int(f[3], 0, cfg.n_mun, &mun)) {
        if (mun == 0) {
            repartir(tipo, qtd);
            snprintf(detalhe, sizeof detalhe, "%d %s(s) recebidas e divididas entre os %d municípios", qtd,
                     NOME_TIPO[tipo], cfg.n_mun);
        } else {
            mudar_estoque(mun, tipo, qtd);
            pedido_pendente[mun][tipo] = 0;
            snprintf(detalhe, sizeof detalhe, "%d %s(s) de reposição para %s", qtd, NOME_TIPO[tipo], cfg.mun[mun]);
        }
        ocorrencia("RECEBIDO_DA_FEDERACAO", mun, NULL, tipo, qtd, detalhe);
    } else if (n == 4 && !strcmp(f[0], "DEBITO") && ler_int(f[1], 1, MAX_MUNICIPIO, &mun) &&
               (tipo = tipo_da_letra(f[2])) >= 0 && ler_int(f[3], 1, 100000000, &qtd)) {
        if (mun <= cfg.n_mun) {
            mudar_estoque(mun, tipo, -qtd);
            snprintf(detalhe, sizeof detalhe, "morador de %s atendido em outro estado: %d %s(s) a menos", cfg.mun[mun],
                     qtd, NOME_TIPO[tipo]);
            ocorrencia("DEBITO_DA_FEDERACAO", mun, NULL, tipo, qtd, detalhe);
        }
    } else {
        printf("[estado %s] a federação mandou algo que não entendi: %s\n", SIGLA_UF[cfg.uf], f[0]);
    }
    pthread_mutex_unlock(&trava);
}

/* Manda a mensagem mais antiga da fila. Devolve 1 se mandou, 0 se a fila está vazia, -1 se a conexão caiu. */
static int enviar_proxima(int fd)
{
    char msg[MAX_LINHA] = "";
    long long id = 0;
    sqlite3_stmt *st;

    pthread_mutex_lock(&trava);
    if (sqlite3_prepare_v2(db, "SELECT id, mensagem FROM fila_federacao ORDER BY id LIMIT 1", -1, &st, NULL) ==
        SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            id = sqlite3_column_int64(st, 0);
            snprintf(msg, sizeof msg, "%s", (const char *)sqlite3_column_text(st, 1));
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&trava);
    if (!id)
        return 0;
    if (!enviar_linha(fd, "%s", msg))
        return -1;
    pthread_mutex_lock(&trava);
    sql("DELETE FROM fila_federacao WHERE id=%lld", id); /* só sai da fila depois de enviada */
    pthread_mutex_unlock(&trava);
    return 1;
}

static void *thread_federacao(void *arg)
{
    int avisou = 0;
    (void)arg;
    for (;;) {
        char linha[MAX_LINHA];
        struct pollfd p;
        int r, fd = conectar(cfg.ip_federacao, cfg.porta_federacao);
        if (fd < 0) {
            if (!avisou)
                printf("[estado %s] federação fora do ar (%s:%d). Vou continuar tentando.\n", SIGLA_UF[cfg.uf],
                       cfg.ip_federacao, cfg.porta_federacao);
            avisou = 1;
            sleep(3);
            continue;
        }
        avisou = 0;
        printf("[estado %s] conectado à federação em %s:%d\n", SIGLA_UF[cfg.uf], cfg.ip_federacao, cfg.porta_federacao);
        if (enviar_linha(fd, "OLA|%d|%d", cfg.uf, cfg.n_mun)) {
            for (;;) {
                while ((r = enviar_proxima(fd)) == 1)
                    ;
                if (r < 0)
                    break;
                p.fd = fd;
                p.events = POLLIN;
                p.revents = 0;
                if (poll(&p, 1, 300) > 0) {
                    if (ler_linha(fd, linha, sizeof linha) < 0)
                        break;
                    tratar_federacao(linha);
                }
            }
        }
        close(fd);
        printf("[estado %s] perdi a conexão com a federação. Vou reconectar.\n", SIGLA_UF[cfg.uf]);
        sleep(3);
    }
    return NULL;
}

/* -------------------------------------------------- thread do teclado */

static void mostrar_estoque(void)
{
    int m, soma[N_TIPOS] = {0, 0};
    pthread_mutex_lock(&trava);
    printf("\nEstoque do estado %s (UF %d)\n", SIGLA_UF[cfg.uf], cfg.uf);
    printf("  nº  %-24s %9s %9s\n", "municipio", "consultas", "vacinas");
    for (m = 1; m <= cfg.n_mun; m++) {
        int c = estoque_de(m, 0), v = estoque_de(m, 1);
        printf("  %2d  %-24s %9d %9d\n", m, cfg.mun[m], c, v);
        soma[0] += c;
        soma[1] += v;
    }
    printf("  Total: %d consultas, %d vacinas. Usuários cadastrados: %lld. Atendimentos: %lld\n\n", soma[0], soma[1],
           sql_numero(0, "SELECT COUNT(*) FROM usuarios"), sql_numero(0, "SELECT COUNT(*) FROM atendimentos"));
    pthread_mutex_unlock(&trava);
}

static void mostrar_ocorrencias(void)
{
    sqlite3_stmt *st;
    pthread_mutex_lock(&trava);
    printf("\nÚltimas 25 ocorrências guardadas no banco do estado:\n");
    if (sqlite3_prepare_v2(db,
                           "SELECT registrado_em, ocorrencia, municipio, detalhe FROM"
                           " (SELECT * FROM ocorrencias ORDER BY id DESC LIMIT 25) ORDER BY id",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            printf("  %s  %-30s município %-2d  %s\n", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                   sqlite3_column_int(st, 2), sqlite3_column_text(st, 3));
        sqlite3_finalize(st);
    }
    printf("\n");
    pthread_mutex_unlock(&trava);
}

static void ajuda(void)
{
    printf("Comandos: 1 = estoque   2 = ocorrências   3 = pedir mais à federação   0 = desligar\n");
}

static void *thread_teclado(void *arg)
{
    char cmd[64];
    (void)arg;
    while (fgets(cmd, sizeof cmd, stdin)) {
        aparar(cmd);
        if (!strcmp(cmd, "1")) mostrar_estoque();
        else if (!strcmp(cmd, "2")) mostrar_ocorrencias();
        else if (!strcmp(cmd, "3")) { /* o estado diz à federação de quanto precisa */
            pthread_mutex_lock(&trava);
            para_federacao("PEDIDO|%d|0|C|%d", cfg.uf, cfg.pedido_inicial[0]);
            para_federacao("PEDIDO|%d|0|V|%d", cfg.uf, cfg.pedido_inicial[1]);
            pthread_mutex_unlock(&trava);
            printf("Pedido mandado: %d consultas e %d vacinas.\n", cfg.pedido_inicial[0], cfg.pedido_inicial[1]);
        } else if (!strcmp(cmd, "0")) {
            pthread_mutex_lock(&trava); /* espera quem estiver gravando terminar */
            sqlite3_close(db);
            printf("[estado %s] desligado\n", SIGLA_UF[cfg.uf]);
            exit(0);
        } else if (cmd[0])
            ajuda();
    }
    return NULL; /* sem teclado (segundo plano): o servidor continua rodando */
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    char arquivo[300];
    struct sockaddr_in end;
    pthread_t t1, t2;
    SVCXPRT *transporte;
    int um = 1, sock;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (!ler_config(argc > 1 ? argv[1] : "config.conf", &cfg))
        return 1;

    snprintf(arquivo, sizeof arquivo, "%s/estado_%s.db", cfg.pasta, SIGLA_UF[cfg.uf]);
    if (sqlite3_open(arquivo, &db) != SQLITE_OK) {
        fprintf(stderr, "Não consegui abrir o banco %s\n", arquivo);
        return 1;
    }
    criar_banco();
    if (!sql_numero(0, "SELECT 1 FROM meta WHERE chave='pedido_inicial'")) {
        /* primeira vez: diz à federação de quanto o estado precisa */
        para_federacao("PEDIDO|%d|0|C|%d", cfg.uf, cfg.pedido_inicial[0]);
        para_federacao("PEDIDO|%d|0|V|%d", cfg.uf, cfg.pedido_inicial[1]);
        sql("INSERT INTO meta VALUES ('pedido_inicial', 'feito')");
    }

    /* Servidor RPC em porta fixa (a do config), sem depender do rpcbind. */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &um, sizeof um);
    memset(&end, 0, sizeof end);
    end.sin_family = AF_INET;
    end.sin_addr.s_addr = htonl(INADDR_ANY);
    end.sin_port = htons((uint16_t)cfg.porta_estado);
    if (sock < 0 || bind(sock, (struct sockaddr *)&end, sizeof end) < 0 || listen(sock, 64) < 0) {
        fprintf(stderr, "Não consegui abrir a porta %d (o estado já está rodando?)\n", cfg.porta_estado);
        return 1;
    }
    transporte = svctcp_create(sock, 0, 0);
    if (!transporte || !svc_register(transporte, SAUDE_PROG, SAUDE_VERS, saude_prog_1, 0)) {
        fprintf(stderr, "Não consegui iniciar o servidor RPC\n");
        return 1;
    }

    printf("[estado %s] no ar. RPC na porta %d, %d municípios, banco em %s\n", SIGLA_UF[cfg.uf], cfg.porta_estado,
           cfg.n_mun, arquivo);
    ajuda();
    pthread_create(&t1, NULL, thread_federacao, NULL);
    pthread_create(&t2, NULL, thread_teclado, NULL);
    svc_run(); /* fica aqui atendendo os municípios */
    return 1;
}
