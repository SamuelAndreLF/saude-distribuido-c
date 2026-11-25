/* saude.x - as funções que o MUNICÍPIO chama no ESTADO por RPC.
 *
 * O rpcgen lê este arquivo e gera:
 *   saude.h       tipos e protótipos
 *   saude_xdr.c   conversão dos dados para a rede
 *   saude_clnt.c  lado do cliente (município)
 *   saude_svc.c   lado do servidor (estado)
 */

const TAM_NOME = 61;
const TAM_DATA = 11;   /* AAAA-MM-DD */
const TAM_HASH = 65;   /* SHA-256 em hexadecimal */
const TAM_MSG  = 160;

/* Identificador de 82 bits: usuário (CPF) 64 + estado 5 + município 13.
   Não cabe em 64 bits, então viaja em duas partes:
   alto  = os 18 bits de cima
   baixo = os 64 bits de baixo */
struct id82 {
    unsigned hyper alto;
    unsigned hyper baixo;
};

/* Todo evento do município para o estado tem este formato e leva o hash. */
struct evento {
    int municipio;          /* município que está mandando */
    id82 usuario;           /* quem é a pessoa e de onde ela é */
    int tipo;               /* 0 = consulta, 1 = vacina */
    int quantidade;
    string data<TAM_DATA>;
    string nome<TAM_NOME>;  /* só no cadastro */
    string hash<TAM_HASH>;  /* SHA-256 dos campos acima */
};

struct resposta {
    int codigo;             /* 0, 1 e 2 = deu certo; 10 em diante = recusado (ver comum.h) */
    int estoque_consulta;   /* quanto o município ainda tem */
    int estoque_vacina;
    string mensagem<TAM_MSG>;
};

program SAUDE_PROG {
    version SAUDE_VERS {
        resposta CADASTRAR(evento) = 1;
        resposta ATENDER(evento) = 2;
        resposta AVISAR_FALTA(evento) = 3;
        resposta CONSULTAR_ESTOQUE(evento) = 4;
    } = 1;
} = 0x20000531;
