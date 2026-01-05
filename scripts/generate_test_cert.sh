set -e

CERT_DIR="certs"
CERT_FILE="$CERT_DIR/server.crt"
KEY_FILE="$CERT_DIR/server.key"

mkdir -p "$CERT_DIR"

echo "Generating private key..."
openssl genrsa -out "$KEY_FILE" 2048

echo "Generating self-signed certificate..."
openssl req -new -x509 -key "$KEY_FILE" -out "$CERT_FILE" -days 365 -subj "/C=US/ST=State/L=City/O=LoadBalancer/CN=localhost"

echo "Certificate generated successfully!"
echo "Certificate: $CERT_FILE"
echo "Private Key: $KEY_FILE"
echo ""
echo "To use with the load balancer, update config.yaml:"
echo "  listener:"
echo "    tls_enabled: true"
echo "    tls_cert: $CERT_FILE"
echo "    tls_key: $KEY_FILE"

