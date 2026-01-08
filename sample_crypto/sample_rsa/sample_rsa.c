#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bn.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

static void print_help()
{
	printf("Usage: sample_rsa [Options] [value]\n");
	printf("Options:\n");
	printf("  -k <key size>     Key Size [1024/2048/3072/4096/8192]\n");
	printf("  -h                Show this help message\n");
}

void handleErrors(const char *msg) {
    fprintf(stderr, "Error: %s\n", msg);
    ERR_print_errors_fp(stderr);
    exit(1);
}

void print_private_key(RSA *rsa) {
	// 打印私钥
	BIO *bio = BIO_new_fp(stdout, BIO_NOCLOSE);  // 用于将数据打印到标准输出
	if (bio == NULL) {
		fprintf(stderr, "BIO_new_fp failed\n");
		return;
	}
	PEM_write_bio_RSAPrivateKey(bio, rsa, NULL, NULL, 0, NULL, NULL);
	BIO_free(bio);
}

void print_public_key(RSA *rsa) {
	// 打印公钥
	BIO *bio = BIO_new_fp(stdout, BIO_NOCLOSE);  // 用于将数据打印到标准输出
	if (bio == NULL) {
		fprintf(stderr, "BIO_new_fp failed\n");
		return;
	}
	PEM_write_bio_RSA_PUBKEY(bio, rsa);
	BIO_free(bio);
}

int main(int argc, char **argv)
{
	int key_size = 0;
	if (argc != 3) {
		print_help();
		return 0;
	}

	if (0 == strcmp(argv[1], "-k")) {
		key_size = atoi(argv[2]);
		if (key_size != 1024 && key_size != 2048 && key_size != 3072 && key_size != 4096 && key_size != 8192) {
			printf("Not Support Key Size: %d\n", key_size);
			print_help();
			return 0;
		}
	}
	else {
		print_help();
		return 0;
	}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	OSSL_PROVIDER *def_provider = OSSL_PROVIDER_load(NULL, "default");
	if (!def_provider) {
		handleErrors("Failed to load default provider");
	}
#endif

	// 1. 生成 RSA 密钥对
	RSA *rsa = RSA_new();
	BIGNUM *bn = BN_new();
	if (!bn || !rsa) {
		handleErrors("Failed to create BIGNUM or RSA");
	}

	if (BN_set_word(bn, RSA_F4) != 1) {  // RSA_F4 是常用的公钥指数 65537
		BN_free(bn);
		RSA_free(rsa);
		handleErrors("BN_set_word failed");
	}
	if (RSA_generate_key_ex(rsa, key_size, bn, NULL) != 1) {
		BN_free(bn);
		RSA_free(rsa);
		handleErrors("RSA_generate_key_ex failed");
	}

	// 2. 创建 EVP_PKEY（包含完整私钥）
	EVP_PKEY *pkey = EVP_PKEY_new();
    if (!pkey) {
        BN_free(bn);
        RSA_free(rsa);
        handleErrors("EVP_PKEY_new failed");
    }

    // 将 RSA 绑定到 EVP_PKEY
    if (EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
        EVP_PKEY_free(pkey);
        BN_free(bn);
        RSA_free(rsa);
        handleErrors("EVP_PKEY_assign_RSA failed");
    }

    BN_free(bn); // bn 可安全释放

	// 打印公钥私钥
	print_private_key(rsa);
	print_public_key(rsa);

	// 准备明文
	unsigned char plaintext[128] = {0};
	unsigned char ciphertext[2048];
	int ciphertext_len;
	snprintf((char *)plaintext, 128, "The test message for RSA-%d encryption!", key_size);

	// 3. 加密数据
	EVP_PKEY_CTX *enc_ctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (!enc_ctx) handleErrors("EVP_PKEY_CTX_new (encrypt) failed");

	if (EVP_PKEY_encrypt_init(enc_ctx) <= 0)
		handleErrors("EVP_PKEY_encrypt_init failed");

	// 可选：显式设置填充（默认是 PKCS#1 v1.5）
    // EVP_PKEY_CTX_set_rsa_padding(enc_ctx, RSA_PKCS1_OAEP_PADDING);

	size_t outlen;
	if (EVP_PKEY_encrypt(enc_ctx, ciphertext, &outlen, plaintext, strlen((char *)plaintext)) <= 0) {
		handleErrors("EVP_PKEY_encrypt Failed");
	}
	ciphertext_len = outlen;

	printf("\nEncrypted text:\n");
	for (int i = 0; i < ciphertext_len; i++) {
		printf("%02x", ciphertext[i]);
	}
	printf("\n\n");

	EVP_PKEY_CTX_free(enc_ctx);

	// 4. 解密数据
	unsigned char decryptedtext[2048];

	EVP_PKEY_CTX *dec_ctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (EVP_PKEY_decrypt_init(dec_ctx) <= 0) {
		handleErrors("EVP_PKEY_decrypt_init failed");
	}
	// 填充必须与加密一致（默认 PKCS#1 v1.5）
    // EVP_PKEY_CTX_set_rsa_padding(dec_ctx, RSA_PKCS1_OAEP_PADDING);

	size_t decrypted_len;
	if (EVP_PKEY_decrypt(dec_ctx, decryptedtext, &decrypted_len, ciphertext, ciphertext_len) <= 0) {
		handleErrors("EVP_PKEY_decrypt Failed");
	}
	decryptedtext[decrypted_len] = '\0';  // 确保解密后的数据是以 \0 结尾的

	printf("Decrypted text: %s\n", decryptedtext);

	EVP_PKEY_CTX_free(dec_ctx);

	// 清理
	EVP_PKEY_free(pkey); // 自动释放内部 RSA

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    OSSL_PROVIDER_unload(def_provider);
#endif

	return 0;
}

