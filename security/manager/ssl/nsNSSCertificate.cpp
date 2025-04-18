/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsNSSCertificate.h"

#include "CertVerifier.h"
#include "ExtendedValidation.h"
#include "NSSCertDBTrustDomain.h"
#include "certdb.h"
#include "mozilla/Assertions.h"
#include "mozilla/Base64.h"
#include "mozilla/Casting.h"
#include "mozilla/NotNull.h"
#include "mozilla/Unused.h"
#include "nsArray.h"
#include "nsCOMPtr.h"
#include "nsICertificateDialogs.h"
#include "nsIClassInfoImpl.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "nsISupportsPrimitives.h"
#include "nsIURI.h"
#include "nsIX509Cert.h"
#include "nsNSSASN1Object.h"
#include "nsNSSCertHelper.h"
#include "nsNSSCertValidity.h"
#include "nsNSSComponent.h" // for PIPNSS string bundle calls.
#include "nsPK11TokenDB.h"
#include "nsPKCS12Blob.h"
#include "nsProxyRelease.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsUnicharUtils.h"
#include "nspr.h"
#include "pkix/pkixnss.h"
#include "pkix/pkixtypes.h"
#include "pkix/Result.h"
#include "prerror.h"
#include "secasn1.h"
#include "secder.h"
#include "secerr.h"
#include "ssl.h"

#ifdef XP_WIN
#include <winsock.h> // for htonl
#endif

using namespace mozilla;
using namespace mozilla::psm;

extern LazyLogModule gPIPNSSLog;

// This is being stored in an uint32_t that can otherwise
// only take values from nsIX509Cert's list of cert types.
// As nsIX509Cert is frozen, we choose a value not contained
// in the list to mean not yet initialized.
#define CERT_TYPE_NOT_YET_INITIALIZED (1 << 30)

NS_IMPL_ISUPPORTS(nsNSSCertificate,
                  nsIX509Cert,
                  nsISerializable,
                  nsIClassInfo)

static NS_DEFINE_CID(kNSSComponentCID, NS_NSSCOMPONENT_CID);

/*static*/ nsNSSCertificate*
nsNSSCertificate::Create(CERTCertificate* cert)
{
  if (cert)
    return new nsNSSCertificate(cert);
  else
    return new nsNSSCertificate();
}

nsNSSCertificate*
nsNSSCertificate::ConstructFromDER(char* certDER, int derLen)
{
  nsNSSCertificate* newObject = nsNSSCertificate::Create();
  if (newObject && !newObject->InitFromDER(certDER, derLen)) {
    delete newObject;
    newObject = nullptr;
  }

  return newObject;
}

bool
nsNSSCertificate::InitFromDER(char* certDER, int derLen)
{
  if (!certDER || !derLen)
    return false;

  CERTCertificate* aCert = CERT_DecodeCertFromPackage(certDER, derLen);

  if (!aCert)
    return false;

  if (!aCert->dbhandle)
  {
    aCert->dbhandle = CERT_GetDefaultCertDB();
  }

  mCert.reset(aCert);
  GetSubjectAltNames();
  return true;
}

nsNSSCertificate::nsNSSCertificate(CERTCertificate* cert)
  : mCert(nullptr)
  , mPermDelete(false)
  , mCertType(CERT_TYPE_NOT_YET_INITIALIZED)
  , mSubjectAltNames()
{
  if (cert) {
    mCert.reset(CERT_DupCertificate(cert));
    GetSubjectAltNames();
  }
}

nsNSSCertificate::nsNSSCertificate()
  : mCert(nullptr)
  , mPermDelete(false)
  , mCertType(CERT_TYPE_NOT_YET_INITIALIZED)
  , mSubjectAltNames()
{
}

nsNSSCertificate::~nsNSSCertificate()
{
  if (mPermDelete) {
    if (mCertType == nsNSSCertificate::USER_CERT) {
      nsCOMPtr<nsIInterfaceRequestor> cxt = new PipUIContext();
      PK11_DeleteTokenCertAndKey(mCert.get(), cxt);
    } else if (mCert->slot && !PK11_IsReadOnly(mCert->slot)) {
      // If the list of built-ins does contain a non-removable
      // copy of this certificate, our call will not remove
      // the certificate permanently, but rather remove all trust.
      SEC_DeletePermCertificate(mCert.get());
    }
  }
}

nsresult
nsNSSCertificate::GetCertType(uint32_t* aCertType)
{
  if (mCertType == CERT_TYPE_NOT_YET_INITIALIZED) {
     // only determine cert type once and cache it
     mCertType = getCertType(mCert.get());
  }
  *aCertType = mCertType;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIsSelfSigned(bool* aIsSelfSigned)
{
  NS_ENSURE_ARG(aIsSelfSigned);

  *aIsSelfSigned = mCert->isRoot;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIsBuiltInRoot(bool* aIsBuiltInRoot)
{
  NS_ENSURE_ARG(aIsBuiltInRoot);

  pkix::Result rv = IsCertBuiltInRoot(mCert.get(), *aIsBuiltInRoot);
  if (rv != pkix::Result::Success) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult
nsNSSCertificate::MarkForPermDeletion()
{
  // make sure user is logged in to the token
  nsCOMPtr<nsIInterfaceRequestor> ctx = new PipUIContext();

  if (mCert->slot && PK11_NeedLogin(mCert->slot) &&
      !PK11_NeedUserInit(mCert->slot) && !PK11_IsInternal(mCert->slot)) {
    if (SECSuccess != PK11_Authenticate(mCert->slot, true, ctx)) {
      return NS_ERROR_FAILURE;
    }
  }

  mPermDelete = true;
  return NS_OK;
}

/**
 * Appends a pipnss bundle string to the given string.
 *
 * @param nssComponent For accessing the string bundle.
 * @param bundleKey Key for the string to append.
 * @param currentText The text to append to, using commas as separators.
 */
template<size_t N>
void
AppendBundleString(const NotNull<nsCOMPtr<nsINSSComponent>>& nssComponent,
                   const char (&bundleKey)[N],
        /*in/out*/ nsAString& currentText)
{
  nsAutoString bundleString;
  nsresult rv = nssComponent->GetPIPNSSBundleString(bundleKey, bundleString);
  if (NS_FAILED(rv)) {
    return;
  }

  if (!currentText.IsEmpty()) {
    currentText.Append(',');
  }
  currentText.Append(bundleString);
}

NS_IMETHODIMP
nsNSSCertificate::GetKeyUsages(nsAString& text)
{
  text.Truncate();

  nsCOMPtr<nsINSSComponent> nssComponent = do_GetService(kNSSComponentCID);
  if (!nssComponent) {
    return NS_ERROR_FAILURE;
  }

  if (!mCert) {
    return NS_ERROR_FAILURE;
  }

  if (!mCert->extensions) {
    return NS_OK;
  }

  ScopedAutoSECItem keyUsageItem;
  if (CERT_FindKeyUsageExtension(mCert.get(), &keyUsageItem) != SECSuccess) {
    return PORT_GetError() == SEC_ERROR_EXTENSION_NOT_FOUND ? NS_OK
                                                            : NS_ERROR_FAILURE;
  }

  unsigned char keyUsage = 0;
  if (keyUsageItem.len) {
    keyUsage = keyUsageItem.data[0];
  }

  NotNull<nsCOMPtr<nsINSSComponent>> wrappedNSSComponent =
    WrapNotNull(nssComponent);
  if (keyUsage & KU_DIGITAL_SIGNATURE) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUSign", text);
  }
  if (keyUsage & KU_NON_REPUDIATION) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUNonRep", text);
  }
  if (keyUsage & KU_KEY_ENCIPHERMENT) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUEnc", text);
  }
  if (keyUsage & KU_DATA_ENCIPHERMENT) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUDEnc", text);
  }
  if (keyUsage & KU_KEY_AGREEMENT) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUKA", text);
  }
  if (keyUsage & KU_KEY_CERT_SIGN) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUCertSign", text);
  }
  if (keyUsage & KU_CRL_SIGN) {
    AppendBundleString(wrappedNSSComponent, "CertDumpKUCRLSign", text);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetDbKey(nsACString& aDbKey)
{
  return GetDbKey(mCert, aDbKey);
}

nsresult
nsNSSCertificate::GetDbKey(const UniqueCERTCertificate& cert, nsACString& aDbKey)
{
  static_assert(sizeof(uint64_t) == 8, "type size sanity check");
  static_assert(sizeof(uint32_t) == 4, "type size sanity check");
  // The format of the key is the base64 encoding of the following:
  // 4 bytes: {0, 0, 0, 0} (this was intended to be the module ID, but it was
  //                        never implemented)
  // 4 bytes: {0, 0, 0, 0} (this was intended to be the slot ID, but it was
  //                        never implemented)
  // 4 bytes: <serial number length in big-endian order>
  // 4 bytes: <DER-encoded issuer distinguished name length in big-endian order>
  // n bytes: <bytes of serial number>
  // m bytes: <DER-encoded issuer distinguished name>
  nsAutoCString buf;
  const char leadingZeroes[] = {0, 0, 0, 0, 0, 0, 0, 0};
  buf.Append(leadingZeroes, sizeof(leadingZeroes));
  uint32_t serialNumberLen = htonl(cert->serialNumber.len);
  buf.Append(BitwiseCast<const char*, const uint32_t*>(&serialNumberLen),
             sizeof(uint32_t));
  uint32_t issuerLen = htonl(cert->derIssuer.len);
  buf.Append(BitwiseCast<const char*, const uint32_t*>(&issuerLen),
             sizeof(uint32_t));
  buf.Append(BitwiseCast<char*, unsigned char*>(cert->serialNumber.data),
             cert->serialNumber.len);
  buf.Append(BitwiseCast<char*, unsigned char*>(cert->derIssuer.data),
             cert->derIssuer.len);

  return Base64Encode(buf, aDbKey);
}

NS_IMETHODIMP
nsNSSCertificate::GetDisplayName(nsAString& aDisplayName)
{
  aDisplayName.Truncate();

  MOZ_ASSERT(mCert, "mCert should not be null in GetDisplayName");
  if (!mCert) {
    return NS_ERROR_FAILURE;
  }

  UniquePORTString commonName(CERT_GetCommonName(&mCert->subject));
  UniquePORTString organizationalUnitName(CERT_GetOrgUnitName(&mCert->subject));
  UniquePORTString organizationName(CERT_GetOrgName(&mCert->subject));

  bool isBuiltInRoot;
  nsresult rv = GetIsBuiltInRoot(&isBuiltInRoot);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // Only use the nickname for built-in roots where we already have a hard-coded
  // reasonable display name (unfortunately we have to strip off the leading
  // slot identifier followed by a ':'). Otherwise, attempt to use the following
  // in order:
  //  - the common name, if present
  //  - an organizational unit name, if present
  //  - an organization name, if present
  //  - the entire subject distinguished name, if non-empty
  //  - an email address, if one can be found
  // In the unlikely event that none of these fields are present and non-empty
  // (the subject really shouldn't be empty), an empty string is returned.
  nsAutoCString builtInRootNickname;
  if (isBuiltInRoot) {
    nsAutoCString fullNickname(mCert->nickname);
    int32_t index = fullNickname.Find(":");
    if (index != kNotFound) {
      // Substring will gracefully handle the case where index is the last
      // character in the string (that is, if the nickname is just
      // "Builtin Object Token:"). In that case, we'll get an empty string.
      builtInRootNickname = Substring(fullNickname,
                                      AssertedCast<uint32_t>(index + 1));
    }
  }
  const char* nameOptions[] = {
    builtInRootNickname.get(),
    commonName.get(),
    organizationalUnitName.get(),
    organizationName.get(),
    mCert->subjectName,
    mCert->emailAddr
  };

  nsAutoCString nameOption;
  for (auto nameOptionPtr : nameOptions) {
    nameOption.Assign(nameOptionPtr);
    if (nameOption.Length() > 0 && IsUTF8(nameOption)) {
      CopyUTF8toUTF16(nameOption, aDisplayName);
      return NS_OK;
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetEmailAddress(nsAString& aEmailAddress)
{
  if (mCert->emailAddr) {
    CopyUTF8toUTF16(mCert->emailAddr, aEmailAddress);
  } else {
    nsresult rv;
    nsCOMPtr<nsINSSComponent> nssComponent(do_GetService(kNSSComponentCID, &rv));
    if (NS_FAILED(rv) || !nssComponent) {
      return NS_ERROR_FAILURE;
    }
    nssComponent->GetPIPNSSBundleString("CertNoEmailAddress", aEmailAddress);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetEmailAddresses(uint32_t* aLength, char16_t*** aAddresses)
{
  NS_ENSURE_ARG(aLength);
  NS_ENSURE_ARG(aAddresses);

  *aLength = 0;

  const char* aAddr;
  for (aAddr = CERT_GetFirstEmailAddress(mCert.get())
       ;
       aAddr
       ;
       aAddr = CERT_GetNextEmailAddress(mCert.get(), aAddr))
  {
    ++(*aLength);
  }

  *aAddresses = (char16_t**) moz_xmalloc(sizeof(char16_t*) * (*aLength));
  if (!*aAddresses)
    return NS_ERROR_OUT_OF_MEMORY;

  uint32_t iAddr;
  for (aAddr = CERT_GetFirstEmailAddress(mCert.get()), iAddr = 0
       ;
       aAddr
       ;
       aAddr = CERT_GetNextEmailAddress(mCert.get(), aAddr), ++iAddr)
  {
    (*aAddresses)[iAddr] = ToNewUnicode(NS_ConvertUTF8toUTF16(aAddr));
  }

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::ContainsEmailAddress(const nsAString& aEmailAddress,
                                       bool* result)
{
  NS_ENSURE_ARG(result);
  *result = false;

  const char* aAddr = nullptr;
  for (aAddr = CERT_GetFirstEmailAddress(mCert.get())
       ;
       aAddr
       ;
       aAddr = CERT_GetNextEmailAddress(mCert.get(), aAddr))
  {
    NS_ConvertUTF8toUTF16 certAddr(aAddr);
    ToLowerCase(certAddr);

    nsAutoString testAddr(aEmailAddress);
    ToLowerCase(testAddr);

    if (certAddr == testAddr)
    {
      *result = true;
      break;
    }

  }

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetCommonName(nsAString& aCommonName)
{
  aCommonName.Truncate();
  if (mCert) {
    UniquePORTString commonName(CERT_GetCommonName(&mCert->subject));
    if (commonName) {
      aCommonName = NS_ConvertUTF8toUTF16(commonName.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetOrganization(nsAString& aOrganization)
{
  aOrganization.Truncate();
  if (mCert) {
    UniquePORTString organization(CERT_GetOrgName(&mCert->subject));
    if (organization) {
      aOrganization = NS_ConvertUTF8toUTF16(organization.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIssuerCommonName(nsAString& aCommonName)
{
  aCommonName.Truncate();
  if (mCert) {
    UniquePORTString commonName(CERT_GetCommonName(&mCert->issuer));
    if (commonName) {
      aCommonName = NS_ConvertUTF8toUTF16(commonName.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIssuerOrganization(nsAString& aOrganization)
{
  aOrganization.Truncate();
  if (mCert) {
    UniquePORTString organization(CERT_GetOrgName(&mCert->issuer));
    if (organization) {
      aOrganization = NS_ConvertUTF8toUTF16(organization.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIssuerOrganizationUnit(nsAString& aOrganizationUnit)
{
  aOrganizationUnit.Truncate();
  if (mCert) {
    UniquePORTString organizationUnit(CERT_GetOrgUnitName(&mCert->issuer));
    if (organizationUnit) {
      aOrganizationUnit = NS_ConvertUTF8toUTF16(organizationUnit.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetOrganizationalUnit(nsAString& aOrganizationalUnit)
{
  aOrganizationalUnit.Truncate();
  if (mCert) {
    UniquePORTString orgunit(CERT_GetOrgUnitName(&mCert->subject));
    if (orgunit) {
      aOrganizationalUnit = NS_ConvertUTF8toUTF16(orgunit.get());
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetSubjectName(nsAString& _subjectName)
{
  _subjectName.Truncate();
  if (mCert->subjectName) {
    _subjectName = NS_ConvertUTF8toUTF16(mCert->subjectName);
  }
  return NS_OK;
}

// Reads dNSName and iPAddress entries encountered in the subject alternative
// name extension of the certificate and stores them in mSubjectAltNames.
void
nsNSSCertificate::GetSubjectAltNames()
{
  mSubjectAltNames.clear();

  ScopedAutoSECItem altNameExtension;
  SECStatus rv = CERT_FindCertExtension(mCert.get(),
                                        SEC_OID_X509_SUBJECT_ALT_NAME,
                                        &altNameExtension);
  if (rv != SECSuccess) {
    return;
  }
  UniquePLArenaPool arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
  if (!arena) {
    return;
  }
  CERTGeneralName* sanNameList(CERT_DecodeAltNameExtension(arena.get(),
                                                           &altNameExtension));
  if (!sanNameList) {
    return;
  }

  CERTGeneralName* current = sanNameList;
  do {
    nsAutoString name;
    switch (current->type) {
      case certDNSName:
        {
          nsDependentCSubstring nameFromCert(BitwiseCast<char*, unsigned char*>(
                                             current->name.other.data),
                                             current->name.other.len);
          // dNSName fields are defined as type IA5String and thus should
          // be limited to ASCII characters.
          if (IsASCII(nameFromCert)) {
            name.Assign(NS_ConvertASCIItoUTF16(nameFromCert));
            mSubjectAltNames.push_back(name);
          }
        }
        break;

      case certIPAddress:
        {
          char buf[INET6_ADDRSTRLEN];
          PRNetAddr addr;
          if (current->name.other.len == 4) {
            addr.inet.family = PR_AF_INET;
            memcpy(&addr.inet.ip, current->name.other.data,
                   current->name.other.len);
            PR_NetAddrToString(&addr, buf, sizeof(buf));
            name.AssignASCII(buf);
          } else if (current->name.other.len == 16) {
            addr.ipv6.family = PR_AF_INET6;
            memcpy(&addr.ipv6.ip, current->name.other.data,
                   current->name.other.len);
            PR_NetAddrToString(&addr, buf, sizeof(buf));
            name.AssignASCII(buf);
          } else {
            /* invalid IP address */
          }
          if (!name.IsEmpty()) {
            mSubjectAltNames.push_back(name);
          }
          break;
        }

      default: // all other types of names are ignored
        break;
    }
    current = CERT_GetNextGeneralName(current);
  } while (current != sanNameList); // double linked

  return;
}

NS_IMETHODIMP
nsNSSCertificate::GetSubjectAltNames(nsAString& _subjectAltNames)
{
  _subjectAltNames.Truncate();

  for (auto altName : mSubjectAltNames) {
    if (!_subjectAltNames.IsEmpty()) {
      _subjectAltNames.Append(',');
    }
    _subjectAltNames.Append(altName);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetIssuerName(nsAString& _issuerName)
{
  _issuerName.Truncate();
  if (mCert->issuerName) {
    _issuerName = NS_ConvertUTF8toUTF16(mCert->issuerName);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetSerialNumber(nsAString& _serialNumber)
{
  _serialNumber.Truncate();
  UniquePORTString tmpstr(CERT_Hexify(&mCert->serialNumber, 1));
  if (tmpstr) {
    _serialNumber = NS_ConvertASCIItoUTF16(tmpstr.get());
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

nsresult
nsNSSCertificate::GetCertificateHash(nsAString& aFingerprint, SECOidTag aHashAlg)
{
  aFingerprint.Truncate();
  Digest digest;
  nsresult rv = digest.DigestBuf(aHashAlg, mCert->derCert.data,
                                 mCert->derCert.len);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // CERT_Hexify's second argument is an int that is interpreted as a boolean
  UniquePORTString fpStr(CERT_Hexify(const_cast<SECItem*>(&digest.get()), 1));
  if (!fpStr) {
    return NS_ERROR_FAILURE;
  }

  aFingerprint.AssignASCII(fpStr.get());
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetSha256Fingerprint(nsAString& aSha256Fingerprint)
{
  return GetCertificateHash(aSha256Fingerprint, SEC_OID_SHA256);
}

NS_IMETHODIMP
nsNSSCertificate::GetSha1Fingerprint(nsAString& _sha1Fingerprint)
{
  return GetCertificateHash(_sha1Fingerprint, SEC_OID_SHA1);
}

NS_IMETHODIMP
nsNSSCertificate::GetTokenName(nsAString& aTokenName)
{
  aTokenName.Truncate();
  if (mCert) {
    // HACK alert
    // When the trust of a builtin cert is modified, NSS copies it into the
    // cert db.  At this point, it is now "managed" by the user, and should
    // not be listed with the builtins.  However, in the collection code
    // used by PK11_ListCerts, the cert is found in the temp db, where it
    // has been loaded from the token.  Though the trust is correct (grabbed
    // from the cert db), the source is wrong.  I believe this is a safe
    // way to work around this.
    if (mCert->slot) {
      char* token = PK11_GetTokenName(mCert->slot);
      if (token) {
        aTokenName = NS_ConvertUTF8toUTF16(token);
      }
    } else {
      nsresult rv;
      nsAutoString tok;
      nsCOMPtr<nsINSSComponent> nssComponent(do_GetService(kNSSComponentCID, &rv));
      if (NS_FAILED(rv)) return rv;
      rv = nssComponent->GetPIPNSSBundleString("InternalToken", tok);
      if (NS_SUCCEEDED(rv))
        aTokenName = tok;
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetSha256SubjectPublicKeyInfoDigest(nsACString& aSha256SPKIDigest)
{
  aSha256SPKIDigest.Truncate();
  Digest digest;
  nsresult rv = digest.DigestBuf(SEC_OID_SHA256, mCert->derPublicKey.data,
                                 mCert->derPublicKey.len);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
  rv = Base64Encode(nsDependentCSubstring(
                      BitwiseCast<char*, unsigned char*>(digest.get().data),
                      digest.get().len),
                    aSha256SPKIDigest);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetRawDER(uint32_t* aLength, uint8_t** aArray)
{
  if (mCert) {
    *aArray = (uint8_t*)moz_xmalloc(mCert->derCert.len);
    if (*aArray) {
      memcpy(*aArray, mCert->derCert.data, mCert->derCert.len);
      *aLength = mCert->derCert.len;
      return NS_OK;
    }
  }
  *aLength = 0;
  return NS_ERROR_FAILURE;
}

CERTCertificate*
nsNSSCertificate::GetCert()
{
  return (mCert) ? CERT_DupCertificate(mCert.get()) : nullptr;
}

NS_IMETHODIMP
nsNSSCertificate::GetValidity(nsIX509CertValidity** aValidity)
{
  NS_ENSURE_ARG(aValidity);

  if (!mCert) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIX509CertValidity> validity = new nsX509CertValidity(mCert);
  validity.forget(aValidity);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetASN1Structure(nsIASN1Object** aASN1Structure)
{
  NS_ENSURE_ARG_POINTER(aASN1Structure);
  if (!NS_IsMainThread()) {
    return NS_ERROR_NOT_SAME_THREAD;
  }
  return CreateASN1Struct(aASN1Structure);
}

NS_IMETHODIMP
nsNSSCertificate::Equals(nsIX509Cert* other, bool* result)
{
  NS_ENSURE_ARG(other);
  NS_ENSURE_ARG(result);

  UniqueCERTCertificate cert(other->GetCert());
  *result = (mCert.get() == cert.get());
  return NS_OK;
}

namespace mozilla {

// TODO(bug 1036065): It seems like we only construct CERTCertLists for the
// purpose of constructing nsNSSCertLists, so maybe we should change this
// function to output an nsNSSCertList instead.
SECStatus
ConstructCERTCertListFromReversedDERArray(
  const mozilla::pkix::DERArray& certArray,
  /*out*/ UniqueCERTCertList& certList)
{
  certList = UniqueCERTCertList(CERT_NewCertList());
  if (!certList) {
    return SECFailure;
  }

  CERTCertDBHandle* certDB(CERT_GetDefaultCertDB()); // non-owning

  size_t numCerts = certArray.GetLength();
  for (size_t i = 0; i < numCerts; ++i) {
    SECItem certDER(UnsafeMapInputToSECItem(*certArray.GetDER(i)));
    UniqueCERTCertificate cert(CERT_NewTempCertificate(certDB, &certDER,
                                                       nullptr, false, true));
    if (!cert) {
      return SECFailure;
    }
    // certArray is ordered with the root first, but we want the resulting
    // certList to have the root last.
    if (CERT_AddCertToListHead(certList.get(), cert.get()) != SECSuccess) {
      return SECFailure;
    }
    Unused << cert.release(); // cert is now owned by certList.
  }

  return SECSuccess;
}

} // namespace mozilla

NS_IMPL_CLASSINFO(nsNSSCertList,
                  nullptr,
                  // inferred from nsIX509Cert
                  nsIClassInfo::THREADSAFE,
                  NS_X509CERTLIST_CID)

NS_IMPL_ISUPPORTS_CI(nsNSSCertList,
                     nsIX509CertList,
                     nsISerializable)

nsNSSCertList::nsNSSCertList(UniqueCERTCertList certList)
{
  if (certList) {
    mCertList = Move(certList);
  } else {
    mCertList = UniqueCERTCertList(CERT_NewCertList());
  }
}

nsNSSCertList::nsNSSCertList()
{
  mCertList = UniqueCERTCertList(CERT_NewCertList());
}

nsNSSCertList*
nsNSSCertList::GetCertList()
{
  return this;
}

NS_IMETHODIMP
nsNSSCertList::AddCert(nsIX509Cert* aCert)
{
  // We need an owning handle when calling nsIX509Cert::GetCert().
  UniqueCERTCertificate cert(aCert->GetCert());
  if (!cert) {
    NS_ERROR("Somehow got nullptr for mCertificate in nsNSSCertificate.");
    return NS_ERROR_FAILURE;
  }

  if (!mCertList) {
    NS_ERROR("Somehow got nullptr for mCertList in nsNSSCertList.");
    return NS_ERROR_FAILURE;
  }
  if (CERT_AddCertToListTail(mCertList.get(), cert.get()) != SECSuccess) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  Unused << cert.release(); // Ownership transferred to the cert list.
  return NS_OK;
}

UniqueCERTCertList
nsNSSCertList::DupCertList(const UniqueCERTCertList& certList)
{
  if (!certList) {
    return nullptr;
  }

  UniqueCERTCertList newList(CERT_NewCertList());
  if (!newList) {
    return nullptr;
  }

  for (CERTCertListNode* node = CERT_LIST_HEAD(certList);
       !CERT_LIST_END(node, certList);
       node = CERT_LIST_NEXT(node)) {
    UniqueCERTCertificate cert(CERT_DupCertificate(node->cert));
    if (!cert) {
      return nullptr;
    }

    if (CERT_AddCertToListTail(newList.get(), cert.get()) != SECSuccess) {
      return nullptr;
    }

    Unused << cert.release(); // Ownership transferred to the cert list.
  }
  return newList;
}

CERTCertList*
nsNSSCertList::GetRawCertList()
{
  return mCertList.get();
}

NS_IMETHODIMP
nsNSSCertList::AsPKCS7Blob(/*out*/ nsACString& result)
{
  MOZ_ASSERT(mCertList);
  if (!mCertList) {
    return NS_ERROR_FAILURE;
  }

  UniqueNSSCMSMessage cmsg(NSS_CMSMessage_Create(nullptr));
  if (!cmsg) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("nsNSSCertList::AsPKCS7Blob - can't create CMS message"));
    return NS_ERROR_OUT_OF_MEMORY;
  }

  UniqueNSSCMSSignedData sigd(nullptr);
  nsresult rv = ForEachCertificateInChain(
    [&cmsg, &sigd] (nsCOMPtr<nsIX509Cert> aCert, bool /*unused*/,
            /*out*/ bool& /*unused*/) {
      // We need an owning handle when calling nsIX509Cert::GetCert().
      UniqueCERTCertificate nssCert(aCert->GetCert());
      if (!sigd) {
        sigd.reset(NSS_CMSSignedData_CreateCertsOnly(cmsg.get(), nssCert.get(),
                                                     false));
        if (!sigd) {
          MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
                  ("nsNSSCertList::AsPKCS7Blob - can't create SignedData"));
          return NS_ERROR_FAILURE;
        }
      } else if (NSS_CMSSignedData_AddCertificate(sigd.get(), nssCert.get())
                   != SECSuccess) {
        MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
                ("nsNSSCertList::AsPKCS7Blob - can't add cert"));
        return NS_ERROR_FAILURE;
      }
      return NS_OK;
  });
  if (NS_FAILED(rv)) {
    return rv;
  }

  NSSCMSContentInfo* cinfo = NSS_CMSMessage_GetContentInfo(cmsg.get());
  if (NSS_CMSContentInfo_SetContent_SignedData(cmsg.get(), cinfo, sigd.get())
        != SECSuccess) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("nsNSSCertList::AsPKCS7Blob - can't attach SignedData"));
    return NS_ERROR_FAILURE;
  }
  // cmsg owns sigd now.
  Unused << sigd.release();

  UniquePLArenaPool arena(PORT_NewArena(1024));
  if (!arena) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("nsNSSCertList::AsPKCS7Blob - out of memory"));
    return NS_ERROR_OUT_OF_MEMORY;
  }

  SECItem certP7 = { siBuffer, nullptr, 0 };
  NSSCMSEncoderContext* ecx = NSS_CMSEncoder_Start(cmsg.get(), nullptr, nullptr,
                                                   &certP7, arena.get(), nullptr,
                                                   nullptr, nullptr, nullptr,
                                                   nullptr, nullptr);
  if (!ecx) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("nsNSSCertList::AsPKCS7Blob - can't create encoder"));
    return NS_ERROR_FAILURE;
  }

  if (NSS_CMSEncoder_Finish(ecx) != SECSuccess) {
    MOZ_LOG(gPIPNSSLog, LogLevel::Debug,
            ("nsNSSCertList::AsPKCS7Blob - failed to add encoded data"));
    return NS_ERROR_FAILURE;
  }

  result.Assign(nsDependentCSubstring(reinterpret_cast<const char*>(certP7.data),
                                      certP7.len));
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertList::Write(nsIObjectOutputStream* aStream)
{
  NS_ENSURE_STATE(mCertList);
  nsresult rv = NS_OK;

  // First, enumerate the certs to get the length of the list
  uint32_t certListLen = 0;
  CERTCertListNode* node = nullptr;
  for (node = CERT_LIST_HEAD(mCertList);
       !CERT_LIST_END(node, mCertList);
       node = CERT_LIST_NEXT(node), ++certListLen) {
  }

  // Write the length of the list
  rv = aStream->Write32(certListLen);

  // Repeat the loop, and serialize each certificate
  node = nullptr;
  for (node = CERT_LIST_HEAD(mCertList);
       !CERT_LIST_END(node, mCertList);
       node = CERT_LIST_NEXT(node))
  {
    nsCOMPtr<nsIX509Cert> cert = nsNSSCertificate::Create(node->cert);
    if (!cert) {
      rv = NS_ERROR_OUT_OF_MEMORY;
      break;
    }

    nsCOMPtr<nsISerializable> serializableCert = do_QueryInterface(cert);
    rv = aStream->WriteCompoundObject(serializableCert, NS_GET_IID(nsIX509Cert), true);
    if (NS_FAILED(rv)) {
      break;
    }
  }

  return rv;
}

NS_IMETHODIMP
nsNSSCertList::Read(nsIObjectInputStream* aStream)
{
  NS_ENSURE_STATE(mCertList);
  nsresult rv = NS_OK;

  uint32_t certListLen;
  rv = aStream->Read32(&certListLen);
  if (NS_FAILED(rv)) {
    return rv;
  }

  for(uint32_t i = 0; i < certListLen; ++i) {
    nsCOMPtr<nsISupports> certSupports;
    rv = aStream->ReadObject(true, getter_AddRefs(certSupports));
    if (NS_FAILED(rv)) {
      break;
    }

    nsCOMPtr<nsIX509Cert> cert = do_QueryInterface(certSupports);
    rv = AddCert(cert);
    if (NS_FAILED(rv)) {
      break;
    }
  }

  return rv;
}

NS_IMETHODIMP
nsNSSCertList::GetEnumerator(nsISimpleEnumerator** _retval)
{
  if (!mCertList) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsISimpleEnumerator> enumerator =
    new nsNSSCertListEnumerator(mCertList);

  enumerator.forget(_retval);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertList::Equals(nsIX509CertList* other, bool* result)
{
  NS_ENSURE_ARG(result);
  *result = true;

  nsresult rv;

  nsCOMPtr<nsISimpleEnumerator> selfEnumerator;
  rv = GetEnumerator(getter_AddRefs(selfEnumerator));
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsISimpleEnumerator> otherEnumerator;
  rv = other->GetEnumerator(getter_AddRefs(otherEnumerator));
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsISupports> selfSupports;
  nsCOMPtr<nsISupports> otherSupports;
  while (NS_SUCCEEDED(selfEnumerator->GetNext(getter_AddRefs(selfSupports)))) {
    if (NS_SUCCEEDED(otherEnumerator->GetNext(getter_AddRefs(otherSupports)))) {
      nsCOMPtr<nsIX509Cert> selfCert = do_QueryInterface(selfSupports);
      nsCOMPtr<nsIX509Cert> otherCert = do_QueryInterface(otherSupports);

      bool certsEqual = false;
      rv = selfCert->Equals(otherCert, &certsEqual);
      if (NS_FAILED(rv)) {
        return rv;
      }
      if (!certsEqual) {
        *result = false;
        break;
      }
    } else {
      // other is shorter than self
      *result = false;
      break;
    }
  }

  // Make sure self is the same length as other
  bool otherHasMore = false;
  rv = otherEnumerator->HasMoreElements(&otherHasMore);
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (otherHasMore) {
    *result = false;
  }

  return NS_OK;
}

nsresult
nsNSSCertList::ForEachCertificateInChain(ForEachCertOperation& aOperation)
{
  nsCOMPtr<nsISimpleEnumerator> chainElt;
  nsresult rv = GetEnumerator(getter_AddRefs(chainElt));
  if (NS_FAILED(rv)) {
    return rv;
  }

  // Each chain may have multiple certificates.
  bool hasMore = false;
  rv = chainElt->HasMoreElements(&hasMore);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!hasMore) {
    return NS_OK; // Empty lists are fine
  }

  do {
    nsCOMPtr<nsISupports> certSupports;
    rv = chainElt->GetNext(getter_AddRefs(certSupports));
    if (NS_FAILED(rv)) {
      return rv;
    }

    nsCOMPtr<nsIX509Cert> cert = do_QueryInterface(certSupports, &rv);
    if (NS_FAILED(rv)) {
      return rv;
    }

    rv = chainElt->HasMoreElements(&hasMore);
    if (NS_FAILED(rv)) {
      return rv;
    }

    bool continueLoop = true;
    rv = aOperation(cert, hasMore, continueLoop);
    if (NS_FAILED(rv) || !continueLoop) {
      return rv;
    }
  } while (hasMore);

  return NS_OK;
}

nsresult
nsNSSCertList::SegmentCertificateChain(/* out */ nsCOMPtr<nsIX509Cert>& aRoot,
                          /* out */ nsCOMPtr<nsIX509CertList>& aIntermediates,
                          /* out */ nsCOMPtr<nsIX509Cert>& aEndEntity)
{
  if (aRoot || aIntermediates || aEndEntity) {
    // All passed-in nsCOMPtrs should be empty for the state machine to work
    return NS_ERROR_UNEXPECTED;
  }

  aIntermediates = new nsNSSCertList();

  nsresult rv = ForEachCertificateInChain(
    [&aRoot, &aIntermediates, &aEndEntity] (nsCOMPtr<nsIX509Cert> aCert,
                                            bool hasMore, bool& aContinue) {
      if (!aEndEntity) {
        // This is the end entity
        aEndEntity = aCert;
      } else if (!hasMore) {
        // This is the root
        aRoot = aCert;
      } else {
        // One of (potentially many) intermediates
        if (NS_FAILED(aIntermediates->AddCert(aCert))) {
          return NS_ERROR_OUT_OF_MEMORY;
        }
      }

      return NS_OK;
  });

  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!aRoot || !aEndEntity) {
    // No self-signed (or empty) chains allowed
    return NS_ERROR_INVALID_ARG;
  }

  return NS_OK;
}

nsresult
nsNSSCertList::GetRootCertificate(/* out */ nsCOMPtr<nsIX509Cert>& aRoot)
{
  if (aRoot) {
    return NS_ERROR_UNEXPECTED;
  }
  CERTCertListNode* rootNode = CERT_LIST_TAIL(mCertList);
  if (!rootNode) {
    return NS_ERROR_UNEXPECTED;
  }
  if (CERT_LIST_END(rootNode, mCertList)) {
    // Empty list, leave aRoot empty
    return NS_OK;
  }
  // Duplicates the certificate
  aRoot = nsNSSCertificate::Create(rootNode->cert);
  if (!aRoot) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  return NS_OK;
}

NS_IMPL_ISUPPORTS(nsNSSCertListEnumerator, nsISimpleEnumerator)

nsNSSCertListEnumerator::nsNSSCertListEnumerator(
  const UniqueCERTCertList& certList)
{
  MOZ_ASSERT(certList);
  mCertList = nsNSSCertList::DupCertList(certList);
}

NS_IMETHODIMP
nsNSSCertListEnumerator::HasMoreElements(bool* _retval)
{
  NS_ENSURE_TRUE(mCertList, NS_ERROR_FAILURE);

  *_retval = !CERT_LIST_EMPTY(mCertList);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertListEnumerator::GetNext(nsISupports** _retval)
{
  NS_ENSURE_TRUE(mCertList, NS_ERROR_FAILURE);

  CERTCertListNode* node = CERT_LIST_HEAD(mCertList);
  if (CERT_LIST_END(node, mCertList)) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIX509Cert> nssCert = nsNSSCertificate::Create(node->cert);
  if (!nssCert) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  nssCert.forget(_retval);

  CERT_RemoveCertListNode(node);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::Write(nsIObjectOutputStream* aStream)
{
  NS_ENSURE_STATE(mCert);
  // This field used to be the cached EV status, but it is no longer necessary.
  nsresult rv = aStream->Write32(0);
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = aStream->Write32(mCert->derCert.len);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return aStream->WriteByteArray(mCert->derCert.data, mCert->derCert.len);
}

NS_IMETHODIMP
nsNSSCertificate::Read(nsIObjectInputStream* aStream)
{
  NS_ENSURE_STATE(!mCert);

  // This field is no longer used.
  uint32_t unusedCachedEVStatus;
  nsresult rv = aStream->Read32(&unusedCachedEVStatus);
  if (NS_FAILED(rv)) {
    return rv;
  }

  uint32_t len;
  rv = aStream->Read32(&len);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCString str;
  rv = aStream->ReadBytes(len, getter_Copies(str));
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!InitFromDER(const_cast<char*>(str.get()), len)) {
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetInterfaces(uint32_t* count, nsIID*** array)
{
  *count = 0;
  *array = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetScriptableHelper(nsIXPCScriptable** _retval)
{
  *_retval = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetContractID(nsACString& aContractID)
{
  aContractID.SetIsVoid(true);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetClassDescription(nsACString& aClassDescription)
{
  aClassDescription.SetIsVoid(true);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetClassID(nsCID** aClassID)
{
  *aClassID = (nsCID*) moz_xmalloc(sizeof(nsCID));
  if (!*aClassID)
    return NS_ERROR_OUT_OF_MEMORY;
  return GetClassIDNoAlloc(*aClassID);
}

NS_IMETHODIMP
nsNSSCertificate::GetFlags(uint32_t* aFlags)
{
  *aFlags = nsIClassInfo::THREADSAFE;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSCertificate::GetClassIDNoAlloc(nsCID* aClassIDNoAlloc)
{
  static NS_DEFINE_CID(kNSSCertificateCID, NS_X509CERT_CID);

  *aClassIDNoAlloc = kNSSCertificateCID;
  return NS_OK;
}
