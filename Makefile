mttest: multitouch.c
	gcc multitouch.c -o mttest -I/usr/include/cairo -lcairo -lXi
