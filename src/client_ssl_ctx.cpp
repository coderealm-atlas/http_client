#include "client_ssl_ctx.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509err.h>
#endif

namespace cjj365 {

#ifdef _WIN32

void load_platform_root_certificates(ssl::context& ctx) {
  HCERTSTORE root_store = CertOpenSystemStoreW(static_cast<HCRYPTPROV_LEGACY>(0), L"ROOT");
  if (!root_store) {
    return;
  }

  ::SSL_CTX* ssl_ctx = ctx.native_handle();
  PCCERT_CONTEXT cert_context = nullptr;
  while ((cert_context = CertEnumCertificatesInStore(root_store, cert_context)) != nullptr) {
    const unsigned char* encoded = cert_context->pbCertEncoded;
    X509* x509 = d2i_X509(nullptr, &encoded, static_cast<long>(cert_context->cbCertEncoded));
    if (!x509) {
      ERR_clear_error();
      continue;
    }

    X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx);
    if (X509_STORE_add_cert(store, x509) != 1) {
      unsigned long err = ERR_peek_last_error();
      if (ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        // Ignore duplicate certificate errors but clear other errors as well.
      }
      ERR_clear_error();
    }

    X509_free(x509);
  }

  CertCloseStore(root_store, 0);
}

#endif  // _WIN32

}  // namespace cjj365
