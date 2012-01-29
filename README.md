libevweb - Evented Web server in C
====

`libevweb` is a web server library built off `libevn`

This was split off another (abandoned) project and hasn't yet been thoroughly
tested on it's own. Before the other project was abandoned I was having some
difficulty making sure the user didn't try to use the pointer for the HTTP
connection after it was already closed and freed. I have since come up with a
somewhat vague idea on a way to handle that, but I have not had the time to
implement it. Because it involves a slight change in the API I will refrain
from documentation until I have solidified how to use it.
