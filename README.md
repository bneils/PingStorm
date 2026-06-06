# IPv4 ping map
This project aims to create an image of the global IPv4 address space using just pings. As you can imagine this is quite hard with $2^{32}$ or 4.2 billion pixels that have to be individually pinged.

What drew me to the project isn't that it has never been done before.
Obviously, plenty of individuals and organizations have done it before (The internet has been around for decades). Instead, I wanted to put my programming skills to the test and also learn OS libraries like `pthread`, `epoll`, `mmap`, and `sockets`.

No GenAI was used in the making of this.

## Some challenges
1. Creating that many sockets quickly balloons and hits the OS's user "soft limit". This is because it treats open network sockets as files and assigns a file descriptor to each. To fix this, I simply raise the ceiling at runtime.
2. Having a lot of threads open and a lot of data to write meant I needed a simple way of writing. I used the `mmap` interface to back the 4 GB file to a shared pointer protected by a mutex. 
3. Reserved / private subnets. Almost a tenth of the global IPv4 routable space is reserved. The largest region of it is pretty much just multicast. I converted the subnets from Wikipedia to hex with a script, then used `memset` to fill those areas in with a constant in the ping file and also skipped over them in my code.
4. Task management being a problem became obvious when I had to ask how I was going to manage 10k sockets (known as the 10k problem). My first thought was to  have several threads checking a list of sockets one-by-one. That was slow. I then wandered onto the `poll` function and eventually the superior `epoll` (paired with non-blocking sockets). My code gets the exact descriptors that received data. It also keeps a linked list of active sockets for timeout purposes that it checks in $O(1)$ time. I have no idea it is better than any other event-driven or async framework.
