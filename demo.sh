#!/bin/bash
# Demonstração completa: compila, liga a federação falsa e dois estados, passa
# por todas as regras do trabalho e confere o estoque no fim.
# Usa a pasta dados_demo, então não mexe no banco de verdade (pasta dados).
#
#   ./demo.sh

cd "$(dirname "$0")" || exit 1
make -s || exit 1

export SAUDE_PASTA=dados_demo
rm -rf dados_demo
mkdir dados_demo
TECLADO=$(mktemp -d)
mkfifo "$TECLADO/go" "$TECLADO/mg"

./federacao_falsa config.conf &
FED=$!
sleep 0.5
./estado config.conf < "$TECLADO/go" > >(tee dados_demo/saida_go.txt) &
GO=$!
exec 3> "$TECLADO/go"
./estado config_outro_estado.conf < "$TECLADO/mg" > >(tee dados_demo/saida_mg.txt) &
MG=$!
exec 4> "$TECLADO/mg"
trap 'kill $FED $GO $MG 2>/dev/null; rm -rf "$TECLADO"' EXIT
sleep 2

passo() { sleep 0.3; echo; echo "=== $* ==="; }

# CPFs válidos usados no teste
ANA=52998224725; CARLOS=11144477735; BRUNO=12345678909; DIEGO=39053344705

passo "1. Goiânia (município 1) cadastra a Ana"
printf '1\n%s\nAna Silva\n0\n' $ANA | ./municipio 1

passo "2. Mesmo CPF de novo no mesmo município: a chave é única, o estado recusa"
printf '1\n%s\nOutra Pessoa\n0\n' $ANA | ./municipio 1

passo "3. Anápolis (3) cadastra o Carlos e aplica vacina nele (caso normal)"
printf '1\n%s\nCarlos Souza\n2\n1\n%s\n\n\n\n\n4\n0\n' $CARLOS $CARLOS | ./municipio 3

passo "4. Itaberaí (10) faz consulta para a Ana, que mora em Goiânia (1)"
printf '2\n2\n%s\n\n1\n\n\n0\n' $ANA | ./municipio 10

passo "5. Minas (UF 12), município 3, cadastra o Bruno"
printf '1\n%s\nBruno Costa\n0\n' $BRUNO | ./municipio 3 config_outro_estado.conf

passo "6. Goiânia (1) aplica vacina no Bruno, que mora em Minas (UF 12, município 3)"
printf '2\n1\n%s\n12\n3\n\n\n0\n' $BRUNO | ./municipio 1

passo "7. Validação do que é digitado: CPF errado, UF 99, quantidade 50, data 31/02/2026, nome com número"
printf '2\n9\n1\n11111111111\n52998224724\n%s\n99\n\n\n50\n\n31/02/2026\n\n1\n%s\nJo4o\n0\n' $ANA $ANA | ./municipio 1

passo "8. Pessoa do nosso estado que não está cadastrada: o estado recusa"
printf '2\n1\n%s\n\n\n\n\n0\n' $DIEGO | ./municipio 1

passo "9. Evento com o hash SHA-256 trocado: o estado recusa"
./municipio 1 --hash-errado

passo "10. Luziânia (5) gasta as 50 vacinas e tenta mais uma: falta, e o estado pede reposição"
{ printf '1\n%s\nDiego Lima\n' $DIEGO; for i in 1 2 3 4 5 6; do printf '2\n1\n%s\n\n\n10\n\n' $DIEGO; done; printf '0\n'; } | ./municipio 5 | tail -4
sleep 1
printf '3\n0\n' | ./municipio 5

passo "Estoque e ocorrências no estado de Goiás, e estoque em Minas"
echo 1 >&3; echo 2 >&3
sleep 1
echo 1 >&4
sleep 1
echo 0 >&3; echo 0 >&4
sleep 0.5

passo "CONFERÊNCIA"
erros=0
conferir() { # arquivo, linha esperada (nº, consultas, vacinas), texto
    if grep -qE "^ +$2 +[A-Za-z ]+ +$3 +$4\$" "dados_demo/$1"; then echo "  [ok] $5"; else echo "  [ERRADO] $5"; erros=1; fi
}
conferir saida_go.txt 1  49 49 "Goiânia: 49 consultas (repôs Itaberaí); vacina do Bruno voltou pela federação, a da Ana no passo 7 não"
conferir saida_go.txt 3  50 49 "Anápolis: 49 vacinas (atendimento normal, não repõe)"
conferir saida_go.txt 5  50 10 "Luziânia: gastou as 50 vacinas e recebeu 10 de reposição"
conferir saida_go.txt 10 50 50 "Itaberaí: 50 consultas (atendeu a Ana e foi reposto)"
conferir saida_mg.txt 3  50 49 "Minas, município 3: 49 vacinas (Bruno vacinou em Goiás)"
grep -q "HASH_INVALIDO" dados_demo/saida_go.txt && echo "  [ok] hash errado virou ocorrência no estado" || { echo "  [ERRADO] hash"; erros=1; }
exit $erros
