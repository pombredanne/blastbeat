The BlastBeat server
=========

BlastBeat is an high-performance HTTP/HTTPS proxy for new generation web apps (websockets, comet...).

It seats on front of your apps and will forward requests to them via a simple ZeroMQ protocol.

Each request will generate a ZeroMQ multipart message for your backends.

The message is composed of 3 parts: session id, message type, message body

'session id' identify a specific connection, a session id (once created by BlastBeat) is always mapped to
the same backend node.

'message type' identify the kind of the message. This is the list of currently defined message types:

headers (request/response contains HTTP headers encoded in uwsgi format)

body (request/response contains raw body)

chunk (response will encode the message in a HTTP chunk, REMEMBER: set the correct Transfer-Encoding in your
header) 

end (request/response close the connection, REMEMBER: BlastBeat supports persistent connections !!!)

websocket (request/response contains a websocket message)

ping (request, check for a backend availability)

pong (response, confirm a backend presence)
