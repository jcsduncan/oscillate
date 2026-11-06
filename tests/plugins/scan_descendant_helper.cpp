#include <cstdlib>
#include <fstream>
#include <sys/types.h>
#include <unistd.h>

int main() {
  const auto* pid_file = std::getenv("OSCILLATE_SCAN_DESCENDANT_PID_FILE");
  if (!pid_file)
    return 2;
  int ready[2];
  if (pipe(ready) != 0)
    return 3;
  const auto child = fork();
  if (child < 0)
    return 4;
  if (child == 0) {
    close(ready[0]);
    std::ofstream output(pid_file);
    output << getpid() << '\n';
    output.close();
    const char signal = 1;
    if (write(ready[1], &signal, 1) != 1)
      _exit(5);
    close(ready[1]);
    for (;;)
      pause();
  }
  close(ready[1]);
  char signal = 0;
  const auto received = read(ready[0], &signal, 1);
  close(ready[0]);
  return received == 1 ? 0 : 6;
}
