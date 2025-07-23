LDLIBS += -lavcodec
LDLIBS += -lavfilter
LDLIBS += -lavformat
LDLIBS += -lavutil
LDLIBS += -lpostproc
LDLIBS += -lswresample
LDLIBS += -lswscale

all: roadlapse

clean:
	rm -f roadlapse
