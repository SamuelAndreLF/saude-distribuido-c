# Precisa de: sudo apt install build-essential rpcbind libtirpc-dev libsqlite3-dev libssl-dev

CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra -O2 -I/usr/include/tirpc
GERADOS = saude.h saude_xdr.c saude_clnt.c saude_svc.c

all: estado municipio federacao_falsa

# O rpcgen gera o código de cliente e servidor a partir do saude.x
$(GERADOS): saude.x
	rm -f $(GERADOS)
	rpcgen -h -o saude.h saude.x
	rpcgen -c -o saude_xdr.c saude.x
	rpcgen -l -o saude_clnt.c saude.x
	rpcgen -m -o saude_svc.c saude.x

# O código gerado é compilado com -w porque o rpcgen deixa variáveis sem uso.
estado: estado.c comum.c comum.h $(GERADOS)
	$(CC) $(CFLAGS) -c -w saude_xdr.c saude_svc.c
	$(CC) $(CFLAGS) -o $@ estado.c comum.c saude_xdr.o saude_svc.o -ltirpc -lsqlite3 -lcrypto -lpthread

municipio: municipio.c comum.c comum.h $(GERADOS)
	$(CC) $(CFLAGS) -c -w saude_xdr.c saude_clnt.c
	$(CC) $(CFLAGS) -o $@ municipio.c comum.c saude_xdr.o saude_clnt.o -ltirpc -lcrypto

federacao_falsa: federacao_falsa.c comum.c comum.h
	$(CC) $(CFLAGS) -o $@ federacao_falsa.c comum.c -lcrypto

clean:
	rm -f estado municipio federacao_falsa *.o $(GERADOS)

.PHONY: all clean
