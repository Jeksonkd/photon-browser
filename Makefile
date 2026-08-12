CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall
PKGS     := gtk+-3.0 webkit2gtk-4.1 json-glib-1.0 libsoup-3.0
CFLAGS   := $(shell pkg-config --cflags $(PKGS))
LIBS     := $(shell pkg-config --libs $(PKGS))

photon-browser: main.cpp
	$(CXX) $(CXXFLAGS) $(CFLAGS) main.cpp -o photon-browser $(LIBS)

clean:
	rm -f photon-browser

.PHONY: clean
