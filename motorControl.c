#include <arpa/inet.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint8_t AES_KEY[16] = {
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B,
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B
};

int encryptMotorSpeed(const uint8_t speed, uint8_t out[16]) {
    uint8_t in[16] = {0};
    in[0] = speed;

    int len = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, AES_KEY, NULL);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    int ok = EVP_EncryptUpdate(ctx, out, &len, in, 16);
    EVP_CIPHER_CTX_free(ctx);
    return ok == 1 ? 0 : -1;
}

int main(void) {
    uint8_t speed_value = 0x00;
    uint8_t cipher[16];

    encryptMotorSpeed(speed_value, cipher);

    if(encryptMotorSpeed(speed_value, cipher) != 0) {
        fprintf(stderr, "Verschluesselung fehlgeschlagen\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    inet_pton(AF_INET, "192.168.4.100", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    send(fd, cipher, 16, 0);

    close(fd);
    return 0;
}
