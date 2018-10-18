# I2P Ouiservice Handshake and Speed Ping protocol

# Handshake

When a client starts a tunnel and tunnel is established it sends a handshake as follows:

    HELLO+ClientID+TunnelID

Where 
    ClientID: 32bit integer persistance among all client's tunnels encoded in big endian.
    TunnelID: 32bit integer encoded in big endian.

Then the client waits for the server reply which should be

    HELLO+ServerID+TunnelID

    ServerID: 32bit integer persistance among all server's tunnels encoded in big endian.
    TunnelID: 32bit integer equal to the client tunnel id encoded in big endian.
    
The client closes the connection and mark the tunnel as established.
    
# Speed Ping Protocol

When a client wants to check if the quality of a tunnel it sends a speed ping request. It opens
a connection on a established handshaken tunnel and sends
    
    SPEEDPING+LengthOfTestBlob+TestBlob
    
Where
    LengthofTestBlob: 32bit integer representing the length of the test blob encoded in big endian.
    TestBlob:         byte array of length of LengthofTestBlob
    
The server should reply with

    SPEEDPING+SHA256(TestBlob)
    
The client should verify the correctness of the hash.
