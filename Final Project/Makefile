CC = gcc
OBJS = server.c
COMPILER_FLAGS = -Wall -lrt -lpthread
OBJ_NAME = server

all :    $(OBJS)
	$(CC) $(OBJS) $(COMPILER_FLAGS) -o $(OBJ_NAME)
	$(CC) client.c $(COMPILER_FLAGS) -o client