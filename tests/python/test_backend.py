import socket
import sys

def echo_server(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', port))
    sock.listen(5)
    print(f"Backend server listening on 127.0.0.1:{port}")
    
    while True:
        conn, addr = sock.accept()
        print(f"Connection from {addr}")
        try:
            conn.settimeout(5.0)
            while True:
                data = conn.recv(1024)
                if not data:
                    print(f"Connection closed by peer (no data)")
                    break
                print(f"Received ({len(data)} bytes): {data.decode('utf-8', errors='ignore')}")
                conn.sendall(data)
                print(f"Echoed back to {addr}")
        except socket.timeout:
            print(f"Connection timeout - no data received from {addr}")
        except Exception as e:
            print(f"Error: {e}")
        finally:
            conn.close()
            print(f"Connection closed")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 test_backend.py <port>")
        sys.exit(1)
    echo_server(int(sys.argv[1]))