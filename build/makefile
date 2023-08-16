SRC = ../src/main.cpp
NAME = stride
FLAGS = -o $(NAME) -I ../include/ -std=c++17

debug:
	g++ $(SRC) $(FLAGS) -Wall -Wextra -pedantic
	./$(NAME)
	
release:
	g++ $(SRC) $(FLAGS) -O2 -s

install:
	make release
	sudo mv $(NAME) /usr/bin/$(NAME)