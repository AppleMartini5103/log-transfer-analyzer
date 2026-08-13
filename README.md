# log-transfer-analyzer
Client-server system for transferring and analyzing large log files. A Windows client (Dear ImGui) streams a 500MB log to a Linux C++17 server over TCP. The server parses it on the fly under a 50MB memory cap, skips corrupted lines without crashing, and returns statistics as result.csv.
