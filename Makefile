FLAGS += -w -g

all:
	gcc -o matmult_p matmult_p.c $(FLAGS)
	gcc -o multiply multiply.c $(FLAGS)
	gcc -o matmult_t matmult_t.c $(FLAGS)
	gcc -o matformatter matformatter.c $(FLAGS)
	gcc -o myshell myshell.c -lreadline $(FLAGS)

clean:
	rm matmult_p multiply matmult_t matformatter myshell
