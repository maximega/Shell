FLAGS += -w -g

all:
	gcc -o myshell myshell.c -lreadline $(FLAGS)

clean:
	rm myshell
