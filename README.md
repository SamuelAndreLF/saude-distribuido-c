# Sistema distribuído de saúde (vacinas e consultas)

Trabalho de Sistemas Distribuídos. Nosso grupo é um estado (Goiás) com 10 municípios.
Em C, para Linux (Ubuntu).

São três níveis:

- **Federação**: roda no PC do professor. Manda vacinas e consultas para os estados e faz o remanejamento entre eles.
- **Estado**: é o nosso grupo. Recebe da federação, distribui para os municípios, guarda tudo no banco e registra as ocorrências.
- **Municípios**: 10. Fazem o atendimento e guardam os registros numa pilha em memória (sem banco).

Comunicação:

- Município ↔ Estado: RPC (rpcgen + libtirpc)
- Estado ↔ Federação: socket TCP

Vacina e consulta seguem o mesmo fluxo. No código são um "recurso" com um campo `tipo` (0 = consulta, 1 = vacina).

## Instalar e compilar

    sudo apt install build-essential rpcbind libtirpc-dev libsqlite3-dev libssl-dev
    make

O `make` roda o `rpcgen` em cima do `saude.x` e gera `saude.h`, `saude_xdr.c`, `saude_clnt.c` e `saude_svc.c`.

## Rodar

A demonstração liga tudo sozinha, passa por todas as regras e confere o estoque no fim:

    ./demo.sh

Para usar na mão, um terminal para cada programa:

    ./federacao_falsa        # só enquanto não estiver ligado no PC do professor
    ./estado
    ./municipio 1            # Goiânia
    ./municipio 10           # Itaberaí ... um terminal por município, de 1 a 10

No município aparece o menu: cadastrar morador, atender (vacina ou consulta), ver estoque, ver registros.
No estado: `1` estoque, `2` ocorrências, `3` pedir mais à federação, `0` desligar.

CPFs válidos para testar: 529.982.247-25, 111.444.777-35, 123.456.789-09, 390.533.447-05.

## No dia da apresentação

1. A VM tem que estar com a rede em modo bridge, senão o PC do professor não enxerga a gente.
2. No `config.conf`, trocar `ip_federacao` (e `porta_federacao`) pelos do PC do professor.
3. Se os municípios rodarem em outras máquinas, trocar `ip_estado` nelas pelo IP da máquina do estado.
4. Não ligar a `federacao_falsa`.

## Arquivos

- `saude.x`: as funções RPC que o município chama no estado. O resto depende dele.
- `estado.c`: servidor RPC, banco SQLite, distribuição, ocorrências, socket com a federação, threads e mutex.
- `municipio.c`: cliente RPC, menu de atendimento, validação do que é digitado, pilha de registros.
- `federacao_falsa.c`: servidor de teste que faz o papel da federação.
- `comum.c` / `comum.h`: identificador de 82 bits, hash SHA-256 (OpenSSL), validações, sockets e leitura do config.
- `config.conf`: IP e porta da federação, UF, porta do estado e lista de municípios.
- `config_outro_estado.conf`: um segundo estado (Minas) só para testar o caso de gente de outro estado.
- `demo.sh`: a demonstração.

## Onde está cada regra

**1. Chave única do usuário = id + município + estado.**
Tabela `usuarios` do banco, com chave primária `(cpf, uf, municipio)`. O estado recusa um segundo cadastro igual. Em `estado.c`, funções `criar_banco` e `cadastrar_1_svc`.

**2. Identificador de 82 bits: usuário 64 + estado 5 + município 13.**
Montado com `unsigned __int128` em `comum.c` (`id_montar`, `id_cpf`, `id_uf`, `id_municipio`). Como não cabe em 64 bits, viaja no RPC em duas partes (`struct id82` no `saude.x`). As UFs são numeradas de 0 a 26 em ordem alfabética (Goiás = 8) e os municípios 1, 2, 3... dentro do estado.

**3. Todo evento do município para o estado vai com hash SHA.**
O município calcula o SHA-256 dos campos do evento (`hash_evento` em `comum.c`, usando OpenSSL) e manda junto. O estado recalcula em `conferir` (`estado.c`); se não bater, recusa e grava a ocorrência `HASH_INVALIDO`.

**4. Validação dos dados digitados no município.**
CPF (11 dígitos e dígitos verificadores), código de estado (0 a 26), código de município, tipo, data (tem que existir e não pode ser futura) e quantidade (1 a 10). Em `municipio.c`, funções `perguntar_*`. O estado confere tudo de novo quando o evento chega.

**5. Pessoa de outro município do nosso estado.**
O atendimento fica registrado no município que atendeu (pilha) e no estado. O estado grava a ocorrência `ATENDIMENTO_DE_OUTRO_MUNICIPIO` e repõe a dose para quem atendeu, tirando do município onde a pessoa mora. Em `atender_1_svc`.

**6. Pessoa de outro estado.**
Registra aqui, o estado pega a UF de origem pelos 5 bits do identificador e manda `REMANEJA` para a federação. A federação repõe para a gente e desconta do estado de origem. Em `atender_1_svc` e `tratar_federacao`.

**7. Acabou a vacina no município.**
O município não atende e avisa o estado (`avisar_falta_1_svc`). O estado grava `FALTA_DE_ESTOQUE` e pede reposição à federação (`pedir_reposicao`).

**8. O estado diz à federação de quanto precisa e divide o que recebe.**
Na primeira vez que liga (e no comando `3`) o estado manda `PEDIDO`. O que chega é dividido igualmente entre os municípios (`repartir`).

**9. Arquivo de config central.**
`config.conf`.

**Threads e mutex.**
O estado tem três threads: a do RPC (municípios), a da federação (socket) e a do teclado. As três mexem no mesmo estoque, então tudo que toca no banco passa pelo mutex `trava`.

**Se a federação cair.**
As mensagens para a federação ficam numa fila no banco (`fila_federacao`) e são enviadas quando a conexão voltar. O estado continua atendendo os municípios.

## Mensagens entre estado e federação

Uma linha de texto por mensagem, campos separados por `|`. Tipo: `C` = consulta, `V` = vacina.

O estado manda:

    OLA|uf|n_municipios
    PEDIDO|uf|municipio|tipo|quantidade            (municipio 0 = para o estado todo)
    REMANEJA|uf|municipio|uf_origem|municipio_origem|tipo|quantidade

A federação manda:

    ENVIO|tipo|quantidade|municipio                (municipio 0 = dividir entre todos)
    DEBITO|municipio|tipo|quantidade               (morador nosso foi atendido em outro estado)

**Este formato fomos nós que definimos para testar. Tem que fechar com o professor.** Se o dele for diferente, muda só em `estado.c`: as funções `tratar_federacao`, `pedir_reposicao`, os `para_federacao(...)` e o `OLA` em `thread_federacao`.

## O que falta combinar com o professor

1. O formato das mensagens da federação (acima).
2. A numeração das UFs. Usamos 0 a 26 em ordem alfabética do nome do estado; a turma inteira tem que usar a mesma.
3. Quanto cada estado pede no começo (usamos 500 de cada) e o tamanho do lote de reposição (10).
4. Se o hash é o SHA-256 só dos dados do evento, como fizemos.
