import socket
import sys


def echo_server(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    running = True
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", port))
        sock.listen(5)
        sock.settimeout(1.0)
        print(f"Backend server listening on 127.0.0.1:{port}")

        while running:
            try:
                conn, addr = sock.accept()
                print(f"Connection from {addr}")
                try:
                    conn.settimeout(5.0)
                    while True:
                        data = conn.recv(1024)
                        if not data:
                            print("Connection closed by peer (no data)")
                            break
                        print(
                            f"Received ({len(data)} bytes): {data.decode('utf-8', errors='ignore')}"
                        )
                        conn.sendall(data)
                        print(f"Echoed back to {addr}")
                except socket.timeout:
                    print(f"Connection timeout - no data received from {addr}")
                except Exception as e:
                    print(f"Error: {e}")
                finally:
                    conn.close()
                    print("Connection closed")
            except socket.timeout:
                continue
            except OSError:
                if running:
                    break
    except KeyboardInterrupt:
        print("\nShutting down server...")
        running = False
    finally:
        sock.close()
        print("Server socket closed")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 backend_server.py <port>")
        sys.exit(1)
    echo_server(int(sys.argv[1]))
