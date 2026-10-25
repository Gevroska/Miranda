/**
 * @file tests/unit/test_crypto.cpp
 * @brief Test src/crypto.*.
 */
// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

// lib includes
#include <openssl/x509.h>

// local includes
#include <src/crypto.h>

TEST(CryptoTest, GeneratedCredentialsExposeSubjectAndVerifySignatures) {
  constexpr std::string_view common_name = "Sunshine Test Host";
  constexpr std::string_view payload = "payload";

  auto creds = crypto::gen_creds(common_name, 2048);
  ASSERT_FALSE(creds.x509.empty());
  ASSERT_FALSE(creds.pkey.empty());

  auto cert = crypto::x509(creds.x509);
  auto pkey = crypto::pkey(creds.pkey);
  ASSERT_NE(cert.get(), nullptr);
  ASSERT_NE(pkey.get(), nullptr);

  const auto subject = X509_get_subject_name(cert.get());
  ASSERT_NE(subject, nullptr);

  const auto common_name_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  ASSERT_GE(common_name_index, 0);

  const auto common_name_entry = X509_NAME_get_entry(subject, common_name_index);
  ASSERT_NE(common_name_entry, nullptr);

  const auto common_name_data = X509_NAME_ENTRY_get_data(common_name_entry);
  ASSERT_NE(common_name_data, nullptr);

#if OPENSSL_VERSION_NUMBER >= 0x40100000L
  const auto common_name_length = ASN1_STRING_get_length(common_name_data);
#else
  const auto common_name_length = static_cast<std::size_t>(ASN1_STRING_length(common_name_data));
#endif
  const std::string_view parsed_common_name {
    reinterpret_cast<const char *>(ASN1_STRING_get0_data(common_name_data)),
    common_name_length
  };
  ASSERT_EQ(parsed_common_name, common_name);

  ASSERT_FALSE(crypto::signature(cert).empty());

  const auto signature = crypto::sign256(pkey, payload);
  ASSERT_FALSE(signature.empty());
  ASSERT_TRUE(crypto::verify256(cert, payload, {reinterpret_cast<const char *>(signature.data()), signature.size()}));
}

TEST(CryptoTest, CertificateSignaturePreservesEmbeddedAndTrailingNulBytes) {
  const auto credentials = crypto::gen_creds("Sunshine Binary Signature Fixture", 2048);
  auto certificate = crypto::x509(credentials.x509);
  ASSERT_NE(certificate.get(), nullptr);

  constexpr std::array<unsigned char, 256> expected {0x80, 0x00, 0xFF, 0x42};
  const auto encoded_length = i2d_X509(certificate.get(), nullptr);
  ASSERT_GT(encoded_length, static_cast<int>(expected.size()));
  std::vector<unsigned char> encoded(static_cast<std::size_t>(encoded_length));
  auto *output = encoded.data();
  ASSERT_EQ(i2d_X509(certificate.get(), &output), encoded_length);
  ASSERT_EQ(output, encoded.data() + encoded.size());

  // Replace only the final 2048-bit RSA signature bytes, retaining the DER length fields.
  std::copy(expected.begin(), expected.end(), encoded.data() + encoded.size() - expected.size());
  const auto *input = encoded.data();
  crypto::x509_t binary_certificate {d2i_X509(nullptr, &input, encoded_length)};
  ASSERT_NE(binary_certificate.get(), nullptr);
  ASSERT_EQ(input, encoded.data() + encoded.size());

  const auto signature = crypto::signature(binary_certificate);
  const std::string_view expected_bytes {reinterpret_cast<const char *>(expected.data()), expected.size()};
  EXPECT_EQ(signature.size(), expected.size());
  EXPECT_EQ(signature, expected_bytes);
}

TEST(CryptoTest, CertificateChainRequiresExactPairedIdentity) {
  const auto issuer_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(issuer_credentials);
  auto paired_certificate = crypto::x509(issuer_credentials.x509);
  auto derived_certificate = crypto::x509(derived_credentials.x509);

  crypto::cert_chain_t certificate_chain;
  certificate_chain.add(crypto::x509(issuer_credentials.x509));

  EXPECT_EQ(certificate_chain.verify(paired_certificate.get()), nullptr);
  EXPECT_NE(certificate_chain.verify(derived_certificate.get()), nullptr);
}
