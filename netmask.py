#!/usr/bin/env python3

def hexc(n):
    return '0x' + ('%X' % n).rjust(8, '0')

def big(a,b,c,d):
    return (a<<24) + (b<<16) + (c<<8) + d

subnets = """224.0.0.0/4
240.0.0.0/4
0.0.0.0/8
10.0.0.0/8
127.0.0.0/8
100.64.0.0/10
172.16.0.0/12
198.18.0.0/15
169.254.0.0/16
192.168.0.0/16
192.0.0.0/24
192.0.2.0/24
192.88.99.0/24
198.51.100.0/24
203.0.113.0/24
233.252.0.0/24
255.255.255.255/32"""

def hex_addr_mask(network_addr: str, cidr: int):
  network_addr = hexc(big(*map(int, network_addr.split("."))))
  netmask = hexc(int('1'*cidr + '0'*(32-cidr), 2))
  return network_addr, netmask

def input_addr(addr_cidr: str):
  network_addr, cidr = addr_cidr.split("/")
  cidr = int(cidr)
  network_addr, netmask = hex_addr_mask(network_addr, cidr)

  #print("netmask (hex): ".ljust(24,' '), netmask)
  #print("network address (hex): ".ljust(24, ' '), network_addr)
  #print(f"// {network_addr_}/{cidr}")
  print("{%s, %s}, // %s" % (network_addr, netmask, addr_cidr))

#inp = input("network address (/CIDR): ")
for subnet in subnets.split():
  input_addr(subnet)