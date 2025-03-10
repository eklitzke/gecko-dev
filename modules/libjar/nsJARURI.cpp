/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

#include "nsJARURI.h"
#include "nsNetUtil.h"
#include "nsIIOService.h"
#include "nsIStandardURL.h"
#include "nsCRT.h"
#include "nsIComponentManager.h"
#include "nsIServiceManager.h"
#include "nsIZipReader.h"
#include "nsReadableUtils.h"
#include "nsAutoPtr.h"
#include "nsNetCID.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "mozilla/ipc/URIUtils.h"

using namespace mozilla::ipc;

static NS_DEFINE_CID(kJARURICID, NS_JARURI_CID);

////////////////////////////////////////////////////////////////////////////////

nsJARURI::nsJARURI()
{
}

nsJARURI::~nsJARURI()
{
}

// XXX Why is this threadsafe?
NS_IMPL_ADDREF(nsJARURI)
NS_IMPL_RELEASE(nsJARURI)
NS_INTERFACE_MAP_BEGIN(nsJARURI)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIJARURI)
  NS_INTERFACE_MAP_ENTRY(nsIURI)
  NS_INTERFACE_MAP_ENTRY(nsIURL)
  NS_INTERFACE_MAP_ENTRY(nsIJARURI)
  NS_INTERFACE_MAP_ENTRY(nsISerializable)
  NS_INTERFACE_MAP_ENTRY(nsIClassInfo)
  NS_INTERFACE_MAP_ENTRY(nsINestedURI)
  NS_INTERFACE_MAP_ENTRY(nsIIPCSerializableURI)
  // see nsJARURI::Equals
  if (aIID.Equals(NS_GET_IID(nsJARURI)))
      foundInterface = reinterpret_cast<nsISupports*>(this);
  else
NS_INTERFACE_MAP_END

nsresult
nsJARURI::Init(const char *charsetHint)
{
    mCharsetHint = charsetHint;
    return NS_OK;
}

#define NS_JAR_SCHEME           NS_LITERAL_CSTRING("jar:")
#define NS_JAR_DELIMITER        NS_LITERAL_CSTRING("!/")
#define NS_BOGUS_ENTRY_SCHEME   NS_LITERAL_CSTRING("x:///")

// FormatSpec takes the entry spec (including the "x:///" at the
// beginning) and gives us a full JAR spec.
nsresult
nsJARURI::FormatSpec(const nsACString &entrySpec, nsACString &result,
                     bool aIncludeScheme)
{
    // The entrySpec MUST start with "x:///"
    NS_ASSERTION(StringBeginsWith(entrySpec, NS_BOGUS_ENTRY_SCHEME),
                 "bogus entry spec");

    nsAutoCString fileSpec;
    nsresult rv = mJARFile->GetSpec(fileSpec);
    if (NS_FAILED(rv)) return rv;

    if (aIncludeScheme)
        result = NS_JAR_SCHEME;
    else
        result.Truncate();

    result.Append(fileSpec + NS_JAR_DELIMITER +
                  Substring(entrySpec, 5, entrySpec.Length() - 5));
    return NS_OK;
}

nsresult
nsJARURI::CreateEntryURL(const nsACString& entryFilename,
                         const char* charset,
                         nsIURL** url)
{
    *url = nullptr;
    // Flatten the concatenation, just in case.  See bug 128288
    nsAutoCString spec(NS_BOGUS_ENTRY_SCHEME + entryFilename);
    return NS_MutateURI(NS_STANDARDURLMUTATOR_CONTRACTID)
        .Apply(NS_MutatorMethod(&nsIStandardURLMutator::Init,
                                nsIStandardURL::URLTYPE_NO_AUTHORITY, -1,
                                spec, charset, nullptr, nullptr))
        .Finalize(url);
}

////////////////////////////////////////////////////////////////////////////////
// nsISerializable methods:

NS_IMETHODIMP
nsJARURI::Read(nsIObjectInputStream *aStream)
{
    NS_NOTREACHED("Use nsIURIMutator.read() instead");
    return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult
nsJARURI::ReadPrivate(nsIObjectInputStream *aInputStream)
{
    nsresult rv;

    nsCOMPtr<nsISupports> supports;
    rv = aInputStream->ReadObject(true, getter_AddRefs(supports));
    NS_ENSURE_SUCCESS(rv, rv);

    mJARFile = do_QueryInterface(supports, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aInputStream->ReadObject(true, getter_AddRefs(supports));
    NS_ENSURE_SUCCESS(rv, rv);

    mJAREntry = do_QueryInterface(supports);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aInputStream->ReadCString(mCharsetHint);
    return rv;
}

NS_IMETHODIMP
nsJARURI::Write(nsIObjectOutputStream* aOutputStream)
{
    nsresult rv;

    rv = aOutputStream->WriteCompoundObject(mJARFile, NS_GET_IID(nsIURI),
                                            true);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aOutputStream->WriteCompoundObject(mJAREntry, NS_GET_IID(nsIURL),
                                            true);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aOutputStream->WriteStringZ(mCharsetHint.get());
    return rv;
}

////////////////////////////////////////////////////////////////////////////////
// nsIClassInfo methods:

NS_IMETHODIMP
nsJARURI::GetInterfaces(uint32_t *count, nsIID * **array)
{
    *count = 0;
    *array = nullptr;
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetScriptableHelper(nsIXPCScriptable **_retval)
{
    *_retval = nullptr;
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetContractID(nsACString& aContractID)
{
    aContractID.SetIsVoid(true);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetClassDescription(nsACString& aClassDescription)
{
    aClassDescription.SetIsVoid(true);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetClassID(nsCID * *aClassID)
{
    *aClassID = (nsCID*) moz_xmalloc(sizeof(nsCID));
    if (!*aClassID)
        return NS_ERROR_OUT_OF_MEMORY;
    return GetClassIDNoAlloc(*aClassID);
}

NS_IMETHODIMP
nsJARURI::GetFlags(uint32_t *aFlags)
{
    // XXX We implement THREADSAFE addref/release, but probably shouldn't.
    *aFlags = nsIClassInfo::MAIN_THREAD_ONLY;
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetClassIDNoAlloc(nsCID *aClassIDNoAlloc)
{
    *aClassIDNoAlloc = kJARURICID;
    return NS_OK;
}

////////////////////////////////////////////////////////////////////////////////
// nsIURI methods:

NS_IMETHODIMP
nsJARURI::GetSpec(nsACString &aSpec)
{
    nsAutoCString entrySpec;
    mJAREntry->GetSpec(entrySpec);
    return FormatSpec(entrySpec, aSpec);
}

NS_IMETHODIMP
nsJARURI::GetSpecIgnoringRef(nsACString &aSpec)
{
    nsAutoCString entrySpec;
    mJAREntry->GetSpecIgnoringRef(entrySpec);
    return FormatSpec(entrySpec, aSpec);
}

NS_IMETHODIMP
nsJARURI::GetDisplaySpec(nsACString &aUnicodeSpec)
{
    return GetSpec(aUnicodeSpec);
}

NS_IMETHODIMP
nsJARURI::GetDisplayHostPort(nsACString &aUnicodeHostPort)
{
    return GetHostPort(aUnicodeHostPort);
}

NS_IMETHODIMP
nsJARURI::GetDisplayPrePath(nsACString &aPrePath)
{
    return GetPrePath(aPrePath);
}

NS_IMETHODIMP
nsJARURI::GetDisplayHost(nsACString &aUnicodeHost)
{
    return GetHost(aUnicodeHost);
}

NS_IMETHODIMP
nsJARURI::GetHasRef(bool *result)
{
    return mJAREntry->GetHasRef(result);
}

nsresult
nsJARURI::SetSpecInternal(const nsACString& aSpec)
{
    return SetSpecWithBase(aSpec, nullptr);
}

// Queries this list of interfaces. If none match, it queries mURI.
NS_IMPL_NSIURIMUTATOR_ISUPPORTS(nsJARURI::Mutator,
                                nsIURISetters,
                                nsIURIMutator,
                                nsIURLMutator,
                                nsISerializable,
                                nsIJARURIMutator)

NS_IMETHODIMP
nsJARURI::Mutator::SetFileName(const nsACString& aFileName, nsIURIMutator** aMutator)
{
    if (!mURI) {
        return NS_ERROR_NULL_POINTER;
    }
    if (aMutator) {
        nsCOMPtr<nsIURIMutator> mutator = this;
        mutator.forget(aMutator);
    }
    return mURI->SetFileNameInternal(aFileName);
}

NS_IMETHODIMP
nsJARURI::Mutator::SetFileBaseName(const nsACString& aFileBaseName, nsIURIMutator** aMutator)
{
    if (!mURI) {
        return NS_ERROR_NULL_POINTER;
    }
    if (aMutator) {
        nsCOMPtr<nsIURIMutator> mutator = this;
        mutator.forget(aMutator);
    }
    return mURI->SetFileBaseNameInternal(aFileBaseName);
}

NS_IMETHODIMP
nsJARURI::Mutator::SetFileExtension(const nsACString& aFileExtension, nsIURIMutator** aMutator)
{
    if (!mURI) {
        return NS_ERROR_NULL_POINTER;
    }
    if (aMutator) {
        nsCOMPtr<nsIURIMutator> mutator = this;
        mutator.forget(aMutator);
    }
    return mURI->SetFileExtensionInternal(aFileExtension);
}

NS_IMETHODIMP
nsJARURI::Mutate(nsIURIMutator** aMutator)
{
    RefPtr<nsJARURI::Mutator> mutator = new nsJARURI::Mutator();
    nsresult rv = mutator->InitFromURI(this);
    if (NS_FAILED(rv)) {
        return rv;
    }
    mutator.forget(aMutator);
    return NS_OK;
}

nsresult
nsJARURI::SetSpecWithBase(const nsACString &aSpec, nsIURI* aBaseURL)
{
    nsresult rv;

    nsCOMPtr<nsIIOService> ioServ(do_GetIOService(&rv));
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString scheme;
    rv = ioServ->ExtractScheme(aSpec, scheme);
    if (NS_FAILED(rv)) {
        // not an absolute URI
        if (!aBaseURL)
            return NS_ERROR_MALFORMED_URI;

        RefPtr<nsJARURI> otherJAR;
        aBaseURL->QueryInterface(NS_GET_IID(nsJARURI), getter_AddRefs(otherJAR));
        NS_ENSURE_TRUE(otherJAR, NS_NOINTERFACE);

        mJARFile = otherJAR->mJARFile;

        nsCOMPtr<nsIURI> entry;

        rv = NS_MutateURI(NS_STANDARDURLMUTATOR_CONTRACTID)
               .Apply(NS_MutatorMethod(&nsIStandardURLMutator::Init,
                                       nsIStandardURL::URLTYPE_NO_AUTHORITY,
                                       -1, nsCString(aSpec), mCharsetHint.get(),
                                       otherJAR->mJAREntry, nullptr))
               .Finalize(entry);
        if (NS_FAILED(rv)) {
            return rv;
        }

        mJAREntry = do_QueryInterface(entry);
        if (!mJAREntry)
            return NS_NOINTERFACE;

        return NS_OK;
    }

    NS_ENSURE_TRUE(scheme.EqualsLiteral("jar"), NS_ERROR_MALFORMED_URI);

    nsACString::const_iterator begin, end;
    aSpec.BeginReading(begin);
    aSpec.EndReading(end);

    while (begin != end && *begin != ':')
        ++begin;

    ++begin; // now we're past the "jar:"

    nsACString::const_iterator delim_begin = begin;
    nsACString::const_iterator delim_end = end;
    nsACString::const_iterator frag = begin;

    if (FindInReadable(NS_JAR_DELIMITER, delim_begin, delim_end)) {
        frag = delim_end;
    }
    while (frag != end && (*frag != '#' && *frag != '?')) {
        ++frag;
    }
    if (frag != end) {
        // there was a fragment or query, mark that as the end of the URL to scan
        end = frag;
    }

    // Search backward from the end for the "!/" delimiter. Remember, jar URLs
    // can nest, e.g.:
    //    jar:jar:http://www.foo.com/bar.jar!/a.jar!/b.html
    // This gets the b.html document from out of the a.jar file, that's
    // contained within the bar.jar file.
    // Also, the outermost "inner" URI may be a relative URI:
    //   jar:../relative.jar!/a.html

    delim_begin = begin;
    delim_end = end;

    if (!RFindInReadable(NS_JAR_DELIMITER, delim_begin, delim_end)) {
        return NS_ERROR_MALFORMED_URI;
    }

    rv = ioServ->NewURI(Substring(begin, delim_begin), mCharsetHint.get(),
                        aBaseURL, getter_AddRefs(mJARFile));
    if (NS_FAILED(rv)) return rv;

    // skip over any extra '/' chars
    while (*delim_end == '/')
        ++delim_end;

    aSpec.EndReading(end); // set to the original 'end'
    return SetJAREntry(Substring(delim_end, end));
}

NS_IMETHODIMP
nsJARURI::GetPrePath(nsACString &prePath)
{
    prePath = NS_JAR_SCHEME;
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetScheme(nsACString &aScheme)
{
    aScheme = "jar";
    return NS_OK;
}

nsresult
nsJARURI::SetScheme(const nsACString &aScheme)
{
    // doesn't make sense to set the scheme of a jar: URL
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetUserPass(nsACString &aUserPass)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetUserPass(const nsACString &aUserPass)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetUsername(nsACString &aUsername)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetUsername(const nsACString &aUsername)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetPassword(nsACString &aPassword)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetPassword(const nsACString &aPassword)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetHostPort(nsACString &aHostPort)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetHostPort(const nsACString &aHostPort)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetHost(nsACString &aHost)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetHost(const nsACString &aHost)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetPort(int32_t *aPort)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::SetPort(int32_t aPort)
{
    return NS_ERROR_FAILURE;
}

nsresult
nsJARURI::GetPathQueryRef(nsACString &aPath)
{
    nsAutoCString entrySpec;
    mJAREntry->GetSpec(entrySpec);
    return FormatSpec(entrySpec, aPath, false);
}

nsresult
nsJARURI::SetPathQueryRef(const nsACString &aPath)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetAsciiSpec(nsACString &aSpec)
{
    // XXX Shouldn't this like... make sure it returns ASCII or something?
    return GetSpec(aSpec);
}

NS_IMETHODIMP
nsJARURI::GetAsciiHostPort(nsACString &aHostPort)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::GetAsciiHost(nsACString &aHost)
{
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsJARURI::Equals(nsIURI *other, bool *result)
{
    return EqualsInternal(other, eHonorRef, result);
}

NS_IMETHODIMP
nsJARURI::EqualsExceptRef(nsIURI *other, bool *result)
{
    return EqualsInternal(other, eIgnoreRef, result);
}

// Helper method:
/* virtual */ nsresult
nsJARURI::EqualsInternal(nsIURI *other,
                         nsJARURI::RefHandlingEnum refHandlingMode,
                         bool *result)
{
    *result = false;

    if (!other)
        return NS_OK;	// not equal

    RefPtr<nsJARURI> otherJAR;
    other->QueryInterface(NS_GET_IID(nsJARURI), getter_AddRefs(otherJAR));
    if (!otherJAR)
        return NS_OK;   // not equal

    bool equal;
    nsresult rv = mJARFile->Equals(otherJAR->mJARFile, &equal);
    if (NS_FAILED(rv) || !equal) {
        return rv;   // not equal
    }

    return refHandlingMode == eHonorRef ?
        mJAREntry->Equals(otherJAR->mJAREntry, result) :
        mJAREntry->EqualsExceptRef(otherJAR->mJAREntry, result);
}

NS_IMETHODIMP
nsJARURI::SchemeIs(const char *i_Scheme, bool *o_Equals)
{
    NS_ENSURE_ARG_POINTER(o_Equals);
    if (!i_Scheme) return NS_ERROR_INVALID_ARG;

    if (*i_Scheme == 'j' || *i_Scheme == 'J') {
        *o_Equals = PL_strcasecmp("jar", i_Scheme) ? false : true;
    } else {
        *o_Equals = false;
    }
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::Clone(nsIURI **result)
{
    nsresult rv;

    nsCOMPtr<nsIJARURI> uri;
    rv = CloneWithJARFileInternal(mJARFile, eHonorRef, getter_AddRefs(uri));
    if (NS_FAILED(rv)) return rv;

    uri.forget(result);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::CloneIgnoringRef(nsIURI **result)
{
    nsresult rv;

    nsCOMPtr<nsIJARURI> uri;
    rv = CloneWithJARFileInternal(mJARFile, eIgnoreRef, getter_AddRefs(uri));
    if (NS_FAILED(rv)) return rv;

    uri.forget(result);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::CloneWithNewRef(const nsACString& newRef, nsIURI **result)
{
    nsresult rv;

    nsCOMPtr<nsIJARURI> uri;
    rv = CloneWithJARFileInternal(mJARFile, eReplaceRef, newRef,
                                  getter_AddRefs(uri));
    if (NS_FAILED(rv)) return rv;

    uri.forget(result);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::Resolve(const nsACString &relativePath, nsACString &result)
{
    nsresult rv;

    nsCOMPtr<nsIIOService> ioServ(do_GetIOService(&rv));
    if (NS_FAILED(rv))
      return rv;

    nsAutoCString scheme;
    rv = ioServ->ExtractScheme(relativePath, scheme);
    if (NS_SUCCEEDED(rv)) {
        // then aSpec is absolute
        result = relativePath;
        return NS_OK;
    }

    nsAutoCString resolvedPath;
    mJAREntry->Resolve(relativePath, resolvedPath);

    return FormatSpec(resolvedPath, result);
}

////////////////////////////////////////////////////////////////////////////////
// nsIURL methods:

NS_IMETHODIMP
nsJARURI::GetFilePath(nsACString& filePath)
{
    return mJAREntry->GetFilePath(filePath);
}

nsresult
nsJARURI::SetFilePath(const nsACString& filePath)
{
    return NS_MutateURI(mJAREntry)
             .SetFilePath(filePath)
             .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetQuery(nsACString& query)
{
    return mJAREntry->GetQuery(query);
}

nsresult
nsJARURI::SetQuery(const nsACString& query)
{
    return NS_MutateURI(mJAREntry)
             .SetQuery(query)
             .Finalize(mJAREntry);
}

nsresult
nsJARURI::SetQueryWithEncoding(const nsACString& query,
                               const Encoding* encoding)
{
    return NS_MutateURI(mJAREntry)
             .SetQueryWithEncoding(query, encoding)
             .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetRef(nsACString& ref)
{
    return mJAREntry->GetRef(ref);
}

nsresult
nsJARURI::SetRef(const nsACString& ref)
{
    return NS_MutateURI(mJAREntry)
             .SetRef(ref)
             .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetDirectory(nsACString& directory)
{
    return mJAREntry->GetDirectory(directory);
}

NS_IMETHODIMP
nsJARURI::GetFileName(nsACString& fileName)
{
    return mJAREntry->GetFileName(fileName);
}

nsresult
nsJARURI::SetFileNameInternal(const nsACString& fileName)
{
    return NS_MutateURI(mJAREntry)
        .Apply(NS_MutatorMethod(&nsIURLMutator::SetFileName,
                                nsCString(fileName), nullptr))
        .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetFileBaseName(nsACString& fileBaseName)
{
    return mJAREntry->GetFileBaseName(fileBaseName);
}

nsresult
nsJARURI::SetFileBaseNameInternal(const nsACString& fileBaseName)
{
    return NS_MutateURI(mJAREntry)
        .Apply(NS_MutatorMethod(&nsIURLMutator::SetFileBaseName,
                                nsCString(fileBaseName), nullptr))
        .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetFileExtension(nsACString& fileExtension)
{
    return mJAREntry->GetFileExtension(fileExtension);
}

nsresult
nsJARURI::SetFileExtensionInternal(const nsACString& fileExtension)
{
    return NS_MutateURI(mJAREntry)
        .Apply(NS_MutatorMethod(&nsIURLMutator::SetFileExtension,
                                nsCString(fileExtension), nullptr))
        .Finalize(mJAREntry);
}

NS_IMETHODIMP
nsJARURI::GetCommonBaseSpec(nsIURI* uriToCompare, nsACString& commonSpec)
{
    commonSpec.Truncate();

    NS_ENSURE_ARG_POINTER(uriToCompare);

    commonSpec.Truncate();
    nsCOMPtr<nsIJARURI> otherJARURI(do_QueryInterface(uriToCompare));
    if (!otherJARURI) {
        // Nothing in common
        return NS_OK;
    }

    nsCOMPtr<nsIURI> otherJARFile;
    nsresult rv = otherJARURI->GetJARFile(getter_AddRefs(otherJARFile));
    if (NS_FAILED(rv)) return rv;

    bool equal;
    rv = mJARFile->Equals(otherJARFile, &equal);
    if (NS_FAILED(rv)) return rv;

    if (!equal) {
        // See what the JAR file URIs have in common
        nsCOMPtr<nsIURL> ourJARFileURL(do_QueryInterface(mJARFile));
        if (!ourJARFileURL) {
            // Not a URL, so nothing in common
            return NS_OK;
        }
        nsAutoCString common;
        rv = ourJARFileURL->GetCommonBaseSpec(otherJARFile, common);
        if (NS_FAILED(rv)) return rv;

        commonSpec = NS_JAR_SCHEME + common;
        return NS_OK;

    }

    // At this point we have the same JAR file.  Compare the JAREntrys
    nsAutoCString otherEntry;
    rv = otherJARURI->GetJAREntry(otherEntry);
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIURL> url;
    rv = CreateEntryURL(otherEntry, nullptr, getter_AddRefs(url));
    if (NS_FAILED(rv)) return rv;

    nsAutoCString common;
    rv = mJAREntry->GetCommonBaseSpec(url, common);
    if (NS_FAILED(rv)) return rv;

    rv = FormatSpec(common, commonSpec);
    return rv;
}

NS_IMETHODIMP
nsJARURI::GetRelativeSpec(nsIURI* uriToCompare, nsACString& relativeSpec)
{
    GetSpec(relativeSpec);

    NS_ENSURE_ARG_POINTER(uriToCompare);

    nsCOMPtr<nsIJARURI> otherJARURI(do_QueryInterface(uriToCompare));
    if (!otherJARURI) {
        // Nothing in common
        return NS_OK;
    }

    nsCOMPtr<nsIURI> otherJARFile;
    nsresult rv = otherJARURI->GetJARFile(getter_AddRefs(otherJARFile));
    if (NS_FAILED(rv)) return rv;

    bool equal;
    rv = mJARFile->Equals(otherJARFile, &equal);
    if (NS_FAILED(rv)) return rv;

    if (!equal) {
        // We live in different JAR files.  Nothing in common.
        return rv;
    }

    // Same JAR file.  Compare the JAREntrys
    nsAutoCString otherEntry;
    rv = otherJARURI->GetJAREntry(otherEntry);
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIURL> url;
    rv = CreateEntryURL(otherEntry, nullptr, getter_AddRefs(url));
    if (NS_FAILED(rv)) return rv;

    nsAutoCString relativeEntrySpec;
    rv = mJAREntry->GetRelativeSpec(url, relativeEntrySpec);
    if (NS_FAILED(rv)) return rv;

    if (!StringBeginsWith(relativeEntrySpec, NS_BOGUS_ENTRY_SCHEME)) {
        // An actual relative spec!
        relativeSpec = relativeEntrySpec;
    }
    return rv;
}

////////////////////////////////////////////////////////////////////////////////
// nsIJARURI methods:

NS_IMETHODIMP
nsJARURI::GetJARFile(nsIURI* *jarFile)
{
    return GetInnerURI(jarFile);
}

NS_IMETHODIMP
nsJARURI::GetJAREntry(nsACString &entryPath)
{
    nsAutoCString filePath;
    mJAREntry->GetFilePath(filePath);
    NS_ASSERTION(filePath.Length() > 0, "path should never be empty!");
    // Trim off the leading '/'
    entryPath = Substring(filePath, 1, filePath.Length() - 1);
    return NS_OK;
}

nsresult
nsJARURI::SetJAREntry(const nsACString &entryPath)
{
    return CreateEntryURL(entryPath, mCharsetHint.get(),
                          getter_AddRefs(mJAREntry));
}

NS_IMETHODIMP
nsJARURI::CloneWithJARFile(nsIURI *jarFile, nsIJARURI **result)
{
    return CloneWithJARFileInternal(jarFile, eHonorRef, result);
}

nsresult
nsJARURI::CloneWithJARFileInternal(nsIURI *jarFile,
                                   nsJARURI::RefHandlingEnum refHandlingMode,
                                   nsIJARURI **result)
{
  return CloneWithJARFileInternal(jarFile, refHandlingMode, EmptyCString(), result);
}

nsresult
nsJARURI::CloneWithJARFileInternal(nsIURI *jarFile,
                                   nsJARURI::RefHandlingEnum refHandlingMode,
                                   const nsACString& newRef,
                                   nsIJARURI **result)
{
    if (!jarFile) {
        return NS_ERROR_INVALID_ARG;
    }

    nsresult rv;

    nsCOMPtr<nsIURI> newJARFile;
    rv = jarFile->Clone(getter_AddRefs(newJARFile));
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIURI> newJAREntryURI;
    if (refHandlingMode == eHonorRef) {
      rv = mJAREntry->Clone(getter_AddRefs(newJAREntryURI));
    } else if (refHandlingMode == eReplaceRef) {
      rv = mJAREntry->CloneWithNewRef(newRef, getter_AddRefs(newJAREntryURI));
    } else {
      rv = mJAREntry->CloneIgnoringRef(getter_AddRefs(newJAREntryURI));
    }
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIURL> newJAREntry(do_QueryInterface(newJAREntryURI));
    NS_ASSERTION(newJAREntry, "This had better QI to nsIURL!");

    nsJARURI* uri = new nsJARURI();
    NS_ADDREF(uri);
    uri->mJARFile = newJARFile;
    uri->mJAREntry = newJAREntry;
    *result = uri;

    return NS_OK;
}

////////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP
nsJARURI::GetInnerURI(nsIURI **aURI)
{
    nsCOMPtr<nsIURI> uri = mJARFile;
    uri.forget(aURI);
    return NS_OK;
}

NS_IMETHODIMP
nsJARURI::GetInnermostURI(nsIURI** uri)
{
    return NS_ImplGetInnermostURI(this, uri);
}

////////////////////////////////////////////////////////////////////////////////
// nsIIPCSerializableURI methods:

void
nsJARURI::Serialize(URIParams& aParams)
{
    JARURIParams params;

    SerializeURI(mJARFile, params.jarFile());
    SerializeURI(mJAREntry, params.jarEntry());
    params.charset() = mCharsetHint;

    aParams = params;
}

bool
nsJARURI::Deserialize(const URIParams& aParams)
{
    if (aParams.type() != URIParams::TJARURIParams) {
        NS_ERROR("Received unknown parameters from the other process!");
        return false;
    }

    const JARURIParams& params = aParams.get_JARURIParams();

    nsCOMPtr<nsIURI> file = DeserializeURI(params.jarFile());
    if (!file) {
        NS_ERROR("Couldn't deserialize jar file URI!");
        return false;
    }

    nsCOMPtr<nsIURI> entry = DeserializeURI(params.jarEntry());
    if (!entry) {
        NS_ERROR("Couldn't deserialize jar entry URI!");
        return false;
    }

    nsCOMPtr<nsIURL> entryURL = do_QueryInterface(entry);
    if (!entryURL) {
        NS_ERROR("Couldn't QI jar entry URI to nsIURL!");
        return false;
    }

    mJARFile.swap(file);
    mJAREntry.swap(entryURL);
    mCharsetHint = params.charset();

    return true;
}
