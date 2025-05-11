#!/usr/bin/env sh
g++ -c main.cpp -g3 -O0 -std=c++11 -pedantic -Wall -o main.o
g++ main.o -g3 -O0 -std=c++11 -pedantic -Wall -o function_call_graph
