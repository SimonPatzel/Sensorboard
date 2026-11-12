#define _GNU_SOURCE         //für recvmmsg, muss vor #include stehen
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static const uint8_t AES_KEY[16] = {                //Verschlüsselungs-Schlüssel
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B,
    0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B, 0x2B
};

const float maxDotLifetime = 0.5;
const float minDotSpawnIntervall = 0.001;
#define dotBuffer 501 // maxDotLifetime / minDotSpawnIntervall +1;

#define RECV_BATCH 64        //max. Pakete, die pro recvmmsg-Aufruf geholt werden
#define PACKET_SIZE 16       //ein Paket = 4 Punkte à 4 Byte (verschlüsselt, ein AES-Block)
#define RCVBUF_BYTES (1 << 20) //gewünschte Größe des Kernel-Empfangspuffers (1 MB)

typedef struct {
    uint16_t angle;
    uint16_t distance;
    float birth;
}Dot;

Dot dots[dotBuffer];
int dotHead = 0;            //nächste Schreibposition
int dotCount = 0;           //Anzahl gültiger Einträge (max dotBuffer)


//Entschlüsselt die Sensordaten vom ESP und gibt sie als Array aus
static int aes_ecb_decrypt(const uint8_t *in, int len, uint8_t *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int outl = 0, finl = 0, ok = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, AES_KEY, NULL) == 1) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);   //kein Padding
        if (EVP_DecryptUpdate(ctx, out, &outl, in, len) == 1 &&
            EVP_DecryptFinal_ex(ctx, out + outl, &finl) == 1) {
            ok = 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok ? outl + finl : -1;
}



// Gibt den Socket Descriptor zurück
int initWiFi(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Kernel-Empfangspuffer vergrößern (wird durch net.core.rmem_max gedeckelt)
    int rcvbuf = RCVBUF_BYTES;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF");
    }
    int actual = 0; socklen_t alen = sizeof(actual);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &alen);
    printf("Empfangspuffer: %d Byte\n", actual);   //Linux liefert hier den doppelten Wert

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5001);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    printf("Warte auf UDP-Pakete auf Port 5001 ...\n");
    return fd;
}


// Legt die 4 Punkte eines entschlüsselten Pakets mit Zeitstempel im Ringpuffer ab
// rawData[16]: je Punkt 2 Byte Winkel, 2 Byte Abstand (little endian)
void handleSensorData(const uint8_t rawData[16]) {
    for (size_t i = 0; i < 4; i++) {
        uint16_t angle    = (uint16_t)( rawData[0 + i * 4] | (rawData[1 + i * 4] << 8) ); //  / 64; //Winkel, noch durch 64
        uint16_t distance = (uint16_t)( rawData[2 + i * 4] | (rawData[3 + i * 4] << 8) ); //  / 4; //Abstand, noch durch 4
        printf("Punkt %zu: Winkel: %u, Abstand: %u\n", i, angle, distance);
 
        //neuen Punkt im Buffer ablegen
        dots[dotHead] = (Dot){angle, distance};
        dotHead = (dotHead + 1) % dotBuffer;
        if (dotCount < dotBuffer) dotCount++;
    }
}

// Wartet blockierend auf das erste Paket und holt dann alle weiteren,
// die gerade im Socket-Puffer liegen (max. RECV_BATCH).
// Gibt die Anzahl gültig verarbeiteter Pakete zurück
int receiveBatch(int fd) {
    static uint8_t bufs[RECV_BATCH][256];
    struct iovec iov[RECV_BATCH];
    struct mmsghdr msgs[RECV_BATCH];
    memset(msgs, 0, sizeof(msgs));

    for (int i = 0; i < RECV_BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = sizeof(bufs[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    
    int n = recvmmsg(fd, msgs, RECV_BATCH, MSG_WAITFORONE | MSG_DONTWAIT, NULL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;       // aktuell kein Paket da, kein Fehler
        }
        perror("recvmmsg");
        return -1;
    }

    int valid = 0;
    for (int i = 0; i < n; i++) {
        if (msgs[i].msg_len != PACKET_SIZE) {
            printf("ungueltige Laenge (%u), verworfen\n", msgs[i].msg_len);
            continue;
        }
        uint8_t plain[PACKET_SIZE];
        if (aes_ecb_decrypt(bufs[i], PACKET_SIZE, plain) != PACKET_SIZE) {
            printf("Entschluesselung fehlgeschlagen\n");
            continue;
        }
        for (int j=0; j<PACKET_SIZE; j++) printf(" %02X", plain[j]);
        printf("\n");

        handleSensorData(plain);
        valid++;
    }
    return valid;
}


int main(void) {
    int fd = initWiFi();
    if (fd < 0) return 1;

    while(1) {
        if (receiveBatch(fd) < 0) {
            usleep(1000);
        }

        // hier main oder so
    }

    close(fd);
    return 0;
}
