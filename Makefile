# add GPL copyright clause here

#
# Environment setup:
# export WATCOM=/home/sergey/watcom
# export INCLUDE=$WATCOM/h
# PATH=$PATH:$WATCOM/binl

CC = wcl -c
# Open Watcom C Compiler options:
# -mc   compact memory form -f
# -zdp  DS == DGROUP
# -zu   SS != DGROUP
# -s    remove stack overflow checks
# -zp1  set struct packing alignment to 1
# -oi   inline intrinsic functions
# -os   optimize for space
# -1    generate code for 186 or higher
# -fpi87 use 8087 instructions
#COPT = -mc -zdp -zu -s -zp1 -oi -os -1 -fpi87
COPT = -mc
LINK = wcl
# Open Watcom liner options:
# -lr   create a DOS real-mode program
#LOPT = -mc -lr
LOPT = -lr
RM = rm

all:	xiflash.exe

xiflash.exe:	xiflash.obj
	$(LINK) $(LOPT) -fe=$@ -fm=$*.map $<

xiflash.obj:	xiflash.c
	$(CC) $(COPT) -fo=$@ xiflash.c

clean:
	$(RM) *.obj
	$(RM) *.exe
