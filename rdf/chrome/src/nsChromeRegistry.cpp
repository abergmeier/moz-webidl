/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Original Author: David W. Hyatt (hyatt@netscape.com)
 *
 * Contributor(s): 
 */

#include <string.h>
#include "nsCOMPtr.h"
#include "nsIFileSpec.h"
#include "nsSpecialSystemDirectory.h"
#include "nsIChromeRegistry.h"
#include "nsChromeRegistry.h"
#include "nsChromeUIDataSource.h"
#include "nsIRDFDataSource.h"
#include "nsIRDFObserver.h"
#include "nsIRDFRemoteDataSource.h"
#include "nsCRT.h"
#include "rdf.h"
#include "nsIServiceManager.h"
#include "nsIRDFService.h"
#include "nsRDFCID.h"
#include "nsIRDFResource.h"
#include "nsIRDFDataSource.h"
#include "nsIRDFContainer.h"
#include "nsIRDFContainerUtils.h"
#include "nsHashtable.h"
#include "nsString.h"
#include "nsXPIDLString.h"
#include "nsISimpleEnumerator.h"
#include "nsNetUtil.h"
#include "nsFileLocations.h"
#include "nsIFileLocator.h"
#include "nsIXBLService.h"
#include "nsPIDOMWindow.h"
#include "nsIDOMWindow.h"
#include "nsIDOMWindowCollection.h"
#include "nsIDOMLocation.h"
#include "nsIWindowMediator.h"
#include "nsIDocument.h"
#include "nsIDOMDocument.h"
#include "nsIXULPrototypeCache.h"
#include "nsIStyleSheet.h"
#include "nsIHTMLCSSStyleSheet.h"
#include "nsIHTMLStyleSheet.h"
#include "nsIHTMLContentContainer.h"
#include "nsIPresShell.h"
#include "nsIStyleSet.h"
#include "nsISupportsArray.h"
#include "nsICSSLoader.h"
#include "nsIDocumentObserver.h"
#include "nsIXULDocument.h"
#include "nsINameSpaceManager.h"
#include "nsIIOService.h"
#include "nsIResProtocolHandler.h"
#include "nsLayoutCID.h"
#include "prio.h"

static char kChromePrefix[] = "chrome://";
static char kInstalledChromeFileName[] = "installed-chrome.txt";


static NS_DEFINE_CID(kWindowMediatorCID, NS_WINDOWMEDIATOR_CID);
static NS_DEFINE_CID(kRDFServiceCID, NS_RDFSERVICE_CID);
static NS_DEFINE_CID(kRDFXMLDataSourceCID, NS_RDFXMLDATASOURCE_CID);
static NS_DEFINE_CID(kRDFContainerUtilsCID,      NS_RDFCONTAINERUTILS_CID);
static NS_DEFINE_CID(kCSSLoaderCID, NS_CSS_LOADER_CID);

class nsChromeRegistry;

#define CHROME_URI "http://www.mozilla.org/rdf/chrome#"

DEFINE_RDF_VOCAB(CHROME_URI, CHROME, selectedSkin);
DEFINE_RDF_VOCAB(CHROME_URI, CHROME, selectedLocale);
DEFINE_RDF_VOCAB(CHROME_URI, CHROME, baseURL);
DEFINE_RDF_VOCAB(CHROME_URI, CHROME, packages);
DEFINE_RDF_VOCAB(CHROME_URI, CHROME, package);
DEFINE_RDF_VOCAB(CHROME_URI, CHROME, name);

////////////////////////////////////////////////////////////////////////////////

// XXX LOCAL COMPONENT PROBLEM! overlayEnumerator must take two sets of
// arcs rather than one, and must be able to move to the second set after
// finishing the first
class nsOverlayEnumerator : public nsISimpleEnumerator
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSISIMPLEENUMERATOR

    nsOverlayEnumerator(nsISimpleEnumerator *aArcs);
    virtual ~nsOverlayEnumerator();

private:
    nsCOMPtr<nsISimpleEnumerator> mArcs;
};

NS_IMPL_ISUPPORTS1(nsOverlayEnumerator, nsISimpleEnumerator)

nsOverlayEnumerator::nsOverlayEnumerator(nsISimpleEnumerator *aArcs)
{
  NS_INIT_REFCNT();
  mArcs = aArcs;
}

nsOverlayEnumerator::~nsOverlayEnumerator()
{
}

NS_IMETHODIMP nsOverlayEnumerator::HasMoreElements(PRBool *aIsTrue)
{
  return mArcs->HasMoreElements(aIsTrue);
}

NS_IMETHODIMP nsOverlayEnumerator::GetNext(nsISupports **aResult)
{
  nsresult rv;
  *aResult = nsnull;

  if (!mArcs)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsISupports> supports;
  mArcs->GetNext(getter_AddRefs(supports));

  nsCOMPtr<nsIRDFLiteral> value = do_QueryInterface(supports, &rv);
  if (NS_FAILED(rv))
    return NS_OK;

  const PRUnichar* valueStr;
  rv = value->GetValueConst(&valueStr);
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsIURL> url;
  
  rv = nsComponentManager::CreateInstance("component://netscape/network/standard-url",
                                          nsnull,
                                          NS_GET_IID(nsIURL),
                                          getter_AddRefs(url));

  if (NS_FAILED(rv))
    return NS_OK;

  nsCAutoString str; str.AssignWithConversion(valueStr);
  url->SetSpec(str);
  
  nsCOMPtr<nsISupports> sup;
  sup = do_QueryInterface(url, &rv);
  if (NS_FAILED(rv))
    return NS_OK;

  *aResult = sup;
  NS_ADDREF(*aResult);

  return NS_OK;
}


////////////////////////////////////////////////////////////////////////////////

nsChromeRegistry::nsChromeRegistry()
{
	NS_INIT_REFCNT();

  mInstallInitialized = PR_FALSE;
  mProfileInitialized = PR_FALSE;
  mDataSourceTable = nsnull;

  nsresult rv;
  rv = nsServiceManager::GetService(kRDFServiceCID,
                                    NS_GET_IID(nsIRDFService),
                                    (nsISupports**)&mRDFService);
  NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF service");

  rv = nsServiceManager::GetService(kRDFContainerUtilsCID,
                                    NS_GET_IID(nsIRDFContainerUtils),
                                    (nsISupports**)&mRDFContainerUtils);
  NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF container utils");

  if (mRDFService) {
    rv = mRDFService->GetResource(kURICHROME_selectedSkin, getter_AddRefs(mSelectedSkin));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");

    rv = mRDFService->GetResource(kURICHROME_selectedLocale, getter_AddRefs(mSelectedLocale));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");

    rv = mRDFService->GetResource(kURICHROME_baseURL, getter_AddRefs(mBaseURL));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");

    rv = mRDFService->GetResource(kURICHROME_packages, getter_AddRefs(mPackages));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");

    rv = mRDFService->GetResource(kURICHROME_package, getter_AddRefs(mPackage));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");

    rv = mRDFService->GetResource(kURICHROME_name, getter_AddRefs(mName));
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to get RDF resource");
  }
}

nsChromeRegistry::~nsChromeRegistry()
{
  delete mDataSourceTable;
   
  if (mRDFService) {
    nsServiceManager::ReleaseService(kRDFServiceCID, mRDFService);
    mRDFService = nsnull;
  }

  if (mRDFContainerUtils) {
    nsServiceManager::ReleaseService(kRDFContainerUtilsCID, mRDFContainerUtils);
    mRDFContainerUtils = nsnull;
  }

}

NS_IMPL_ISUPPORTS1(nsChromeRegistry, nsIChromeRegistry);

////////////////////////////////////////////////////////////////////////////////
// nsIChromeRegistry methods:

static nsresult
SplitURL(nsIURI* aChromeURI, nsCString& aPackage, nsCString& aProvider, nsCString& aFile)
{
  // Splits a "chrome:" URL into its package, provider, and file parts.
  // Here are the current portions of a 
  // chrome: url that make up the chrome-
  //
  //     chrome://global/skin/foo?bar
  //     \------/ \----/\---/ \-----/
  //         |       |     |     |
  //         |       |     |     `-- RemainingPortion
  //         |       |     |
  //         |       |     `-- Provider 
  //         |       |
  //         |       `-- Package
  //         |
  //         `-- Always "chrome://"
  //
  //

  nsresult rv;

  char* str;
  rv = aChromeURI->GetSpec(&str);
  if (NS_FAILED(rv)) return rv;

  if (! str)
    return NS_ERROR_INVALID_ARG;

  PRInt32 len = PL_strlen(str);
  nsCAutoString spec = CBufDescriptor(str, PR_FALSE, len + 1, len);

  // We only want to deal with "chrome:" URLs here. We could return
  // an error code if the URL isn't properly prefixed here...
  if (PL_strncmp(spec, kChromePrefix, sizeof(kChromePrefix) - 1) != 0)
    return NS_ERROR_INVALID_ARG;

  // Cull out the "package" string; e.g., "navigator"
  spec.Right(aPackage, spec.Length() - (sizeof(kChromePrefix) - 1));

  PRInt32 idx;
  idx = aPackage.FindChar('/');
  if (idx < 0)
    return NS_OK;

  // Cull out the "provider" string; e.g., "content"
  aPackage.Right(aProvider, aPackage.Length() - (idx + 1));
  aPackage.Truncate(idx);

  idx = aProvider.FindChar('/');
  if (idx < 0) {
    // Force the provider to end with a '/'
    idx = aProvider.Length();
    aProvider.Append('/');
  }

  // Cull out the "file"; e.g., "navigator.xul"
  aProvider.Right(aFile, aProvider.Length() - (idx + 1));
  aProvider.Truncate(idx);

  if (aFile.Length() == 0) {
    // If there is no file, then construct the default file
    aFile = aPackage;

    if (aProvider.Equals("content")) {
        aFile += ".xul";
    }
    else if (aProvider.Equals("skin")) {
        aFile += ".css";
    }
    else if (aProvider.Equals("locale")) {
        aFile += ".dtd";
    }
    else {
        NS_ERROR("unknown provider");
        return NS_ERROR_FAILURE;
    }
  } else {
      // Protect against URIs containing .. that reach up out of the 
      // chrome directory to grant chrome privileges to non-chrome files.
      int depth = 0;
      PRBool sawSlash = PR_TRUE;  // .. at the beginning is suspect as well as /..
      for (const char* p=aFile; *p; p++) {
        if (sawSlash) {
          if (p[0] == '.') {                
            if (p[1] == '.') {
                depth--;    // we have /.., decrement depth. 
            } else if (p[1] == '/') {
                           // we have /./, leave depth alone
            }
          } else if (p[0] != '/') {
              depth++;        // we have /x for some x that is not /
          }
        }
        sawSlash = (p[0] == '/');
        if (depth < 0)
            return NS_ERROR_FAILURE;
      }
  }

  return NS_OK;
}


NS_IMETHODIMP
nsChromeRegistry::Canonify(nsIURI* aChromeURI)
{
  // Canonicalize 'chrome:' URLs. We'll take any 'chrome:' URL
  // without a filename, and change it to a URL -with- a filename;
  // e.g., "chrome://navigator/content" to
  // "chrome://navigator/content/navigator.xul".
  if (! aChromeURI)
      return NS_ERROR_NULL_POINTER;

  nsCAutoString package, provider, file;
  nsresult rv;
  rv = SplitURL(aChromeURI, package, provider, file);
  if (NS_FAILED(rv)) return rv;

  nsCAutoString canonical = kChromePrefix;
  canonical += package;
  canonical += "/";
  canonical += provider;
  canonical += "/";
  canonical += file;

  return aChromeURI->SetSpec(canonical);
}

NS_IMETHODIMP
nsChromeRegistry::ConvertChromeURL(nsIURI* aChromeURL, char** aResult)
{
  nsresult rv = NS_OK;
  NS_ASSERTION(aChromeURL, "null url!");
  if (!aChromeURL)
      return NS_ERROR_NULL_POINTER;

  // First canonify the beast
  Canonify(aChromeURL);

  // Obtain the package, provider and remaining from the URL
  nsCAutoString package, provider, remaining;

  rv = SplitURL(aChromeURL, package, provider, remaining);
  if (NS_FAILED(rv)) return rv;

  if (!mProfileInitialized) {
    // Just setSpec 
    GetInstallRoot(mInstallRoot);
    rv = GetProfileRoot(mProfileRoot);
    if (NS_SUCCEEDED(rv)) {
      // Load the profile search path for skins, content, and locales
      // Prepend them to our list of substitutions.
      mProfileInitialized = mInstallInitialized = PR_TRUE;
      mChromeDataSource = nsnull;
      AddToCompositeDataSource(PR_TRUE);

      LoadStyleSheet(getter_AddRefs(mScrollbarSheet), "chrome://global/skin/scrollbars.css"); 
      // This must always be the last line of profile initialization!

      nsCAutoString userSheetURL;
      GetUserSheetURL(userSheetURL);
      if(!userSheetURL.IsEmpty()) {
        LoadStyleSheet(getter_AddRefs(mUserSheet), userSheetURL);
      }
    }
    else if (!mInstallInitialized) {
      // Load the installed search path for skins, content, and locales
      // Prepend them to our list of substitutions
      mInstallInitialized = PR_TRUE;
      AddToCompositeDataSource(PR_FALSE);

      LoadStyleSheet(getter_AddRefs(mScrollbarSheet), "chrome://global/skin/scrollbars.css"); 
        // This must always be the last line of install initialization!
    }
  }
 
  nsCAutoString finalURL;
  GetBaseURL(package, provider, finalURL);
  if (finalURL.IsEmpty()) {
    // hard-coded fallback
    if (provider.Equals("skin")) {
      finalURL = "resource:/chrome/skins/modern/";
    }
#ifdef XP_MAC
    else {
      finalURL = "resource:/chrome/";
    }
#else
    else if (provider.Equals("locale")) {
      finalURL = "resource:/chrome/locales/en-US/";
    }
    else if (package.Equals("aim")) {
      finalURL = "resource:/chrome/packages/aim/";
    }
    else if (package.Equals("messenger")) {
      finalURL = "resource:/chrome/packages/messenger/";
    }
    else if (package.Equals("global")) {
      finalURL = "resource:/chrome/packages/widget-toolkit/";
    }
    else {
      finalURL = "resource:/chrome/packages/core/";
    }
#endif
  } 

  finalURL += package;
  finalURL += "/";
  finalURL += provider;
  finalURL += "/";
  finalURL += remaining;

  *aResult = nsXPIDLCString::Copy(finalURL);

  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::GetBaseURL(const nsCAutoString& aPackage, const nsCAutoString& aProvider, 
                             nsCAutoString& aBaseURL)
{
  nsCAutoString resourceStr("urn:mozilla:package:");
  resourceStr += aPackage;

  // Obtain the resource.
  nsresult rv = NS_OK;
  nsCOMPtr<nsIRDFResource> resource;
  rv = GetResource(resourceStr, getter_AddRefs(resource));
  if (NS_FAILED(rv)) {
    NS_ERROR("Unable to obtain the package resource.");
    return rv;
  }  

  // Follow the "selectedSkin" or "selectedLocale" arc.
  nsCOMPtr<nsIRDFResource> arc;
  if (aProvider.Equals(nsCAutoString("skin"))) {
    arc = mSelectedSkin;
  }
  else if (aProvider.Equals(nsCAutoString("locale"))) {
    arc = mSelectedLocale;
  }

  if (arc) {
    
    nsCOMPtr<nsIRDFNode> selectedProvider;
    if (NS_FAILED(rv = mChromeDataSource->GetTarget(resource, arc, PR_TRUE, getter_AddRefs(selectedProvider)))) {
      NS_ERROR("Unable to obtain the provider.");
      return rv;
    }

    if (!selectedProvider)
      rv = FindProvider(aPackage, aProvider, arc, getter_AddRefs(selectedProvider));
    if (!selectedProvider)
      return rv;

    resource = do_QueryInterface(selectedProvider);
    if (!resource)
      return NS_ERROR_FAILURE;
  }

  // From this resource, follow the "baseURL" arc.
  nsChromeRegistry::FollowArc(mChromeDataSource,
                              aBaseURL, 
                              resource,
                              mBaseURL);
  return NS_OK;
}

// locate 
NS_IMETHODIMP
nsChromeRegistry::FindProvider(const nsCAutoString& aPackage,
                               const nsCAutoString& aProvider,
                               nsIRDFResource *aArc,
                               nsIRDFNode **aSelectedProvider)
{
  *aSelectedProvider = nsnull;

  nsCAutoString rootStr("urn:mozilla:");
  nsresult rv = NS_OK;

  rootStr += aProvider;
  rootStr += ":root";

  // obtain the provider root resource
  nsCOMPtr<nsIRDFResource> resource;
  rv = GetResource(rootStr, getter_AddRefs(resource));
  if (NS_FAILED(rv)) {
    NS_ERROR("Unable to obtain the package resource.");
    return rv;
  }

  // wrap it in a container
  nsCOMPtr<nsIRDFContainer> container;
  rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                          nsnull,
                                          NS_GET_IID(nsIRDFContainer),
                                          getter_AddRefs(container));
  if (NS_SUCCEEDED(rv))
    rv = container->Init(mChromeDataSource, resource);
  if (NS_FAILED(rv))
    return rv;

  // step through its (seq) arcs
  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(rv = container->GetElements(getter_AddRefs(arcs))))
    return rv;

  PRBool moreElements;
  arcs->HasMoreElements(&moreElements);
  for ( ; moreElements; arcs->HasMoreElements(&moreElements)) {

    // get next arc resource
    nsCOMPtr<nsISupports> supports;
    arcs->GetNext(getter_AddRefs(supports));
    nsCOMPtr<nsIRDFResource> kid = do_QueryInterface(supports);

    if (kid) {
      // get its name
      nsCAutoString providerName;
      nsChromeRegistry::FollowArc(mChromeDataSource, providerName, kid, mName);

      // get its package list
      nsCOMPtr<nsIRDFNode> packageNode;
      nsCOMPtr<nsIRDFResource> packageList;
      if (NS_SUCCEEDED(mChromeDataSource->GetTarget(kid, mPackages, PR_TRUE, getter_AddRefs(packageNode))))
        packageList = do_QueryInterface(packageNode);
      if (!packageList)
        continue;

      // if aPackage is named in kid's package list, select it and we're done
      SelectPackageInProvider(packageList, aPackage, aProvider, providerName,
                            aArc, aSelectedProvider);
      if (aSelectedProvider)
        return NS_OK;
    }
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsChromeRegistry::SelectPackageInProvider(nsIRDFResource *aPackageList,
                                          const nsCAutoString& aPackage,
                                          const nsCAutoString& aProvider,
                                          const nsCAutoString& aProviderName,
                                          nsIRDFResource *aArc,
                                          nsIRDFNode **aSelectedProvider)
{
  *aSelectedProvider = nsnull;

  nsresult rv = NS_OK;

  // wrap aPackageList in a container
  nsCOMPtr<nsIRDFContainer> container;
  rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                          nsnull,
                                          NS_GET_IID(nsIRDFContainer),
                                          getter_AddRefs(container));
  if (NS_SUCCEEDED(rv))
    rv = container->Init(mChromeDataSource, aPackageList);
  if (NS_FAILED(rv))
    return rv;

  // step through its (seq) arcs
  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(rv = container->GetElements(getter_AddRefs(arcs))))
    return rv;

  PRBool moreElements;
  arcs->HasMoreElements(&moreElements);
  for ( ; moreElements; arcs->HasMoreElements(&moreElements)) {

    // get next arc resource
    nsCOMPtr<nsISupports> supports;
    arcs->GetNext(getter_AddRefs(supports));
    nsCOMPtr<nsIRDFResource> kid = do_QueryInterface(supports);

    if (kid) {
      // get its package resource
      nsCOMPtr<nsIRDFNode> packageNode;
      nsCOMPtr<nsIRDFResource> package;
      if (NS_SUCCEEDED(mChromeDataSource->GetTarget(kid, mPackage, PR_TRUE, getter_AddRefs(packageNode))))
        package = do_QueryInterface(packageNode);
      if (!package)
        continue;

      // get its name
      nsCAutoString packageName;
      nsChromeRegistry::FollowArc(mChromeDataSource, packageName, package, mName);

      // select provider assuming it comes from the install directory.
      // XXX we really should be keeping track of whether it's from there,
      // or from the profile
      if (packageName.Equals(aPackage)) {
        nsAutoString providerNameUC;
        nsAutoString packageNameUC;
        providerNameUC.AssignWithConversion(aProviderName);
        packageNameUC.AssignWithConversion(packageName);
        SelectProviderForPackage(aProvider, providerNameUC.GetUnicode(),
                          packageNameUC.GetUnicode(), aArc, PR_FALSE, PR_TRUE);
        *aSelectedProvider = kid;
        NS_ADDREF(*aSelectedProvider);
        return NS_OK;
      }
    }
  }
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP nsChromeRegistry::GetOverlayDataSource(nsIURI *aChromeURL, nsIRDFDataSource **aResult)
{
  *aResult = nsnull;

  nsresult rv;

  if (!mDataSourceTable)
    return NS_OK;

  // Obtain the package, provider and remaining from the URL
  nsCAutoString package, provider, remaining;

  rv = SplitURL(aChromeURL, package, provider, remaining);
  if (NS_FAILED(rv)) return rv;

  // Retrieve the mInner data source.
  nsCAutoString overlayFile = "overlayinfo/";
  overlayFile += package;
  overlayFile += "/";
  overlayFile += provider;
  overlayFile += "/";
  overlayFile += "overlays.rdf";

  // XXX For now, only support install-based overlays (not profile-based overlays)
  return LoadDataSource(overlayFile, aResult, PR_FALSE);
}


NS_IMETHODIMP nsChromeRegistry::GetOverlays(nsIURI *aChromeURL, nsISimpleEnumerator **aResult)
{
  *aResult = nsnull;

  nsresult rv;

  if (!mDataSourceTable)
    return NS_OK;

  nsCOMPtr<nsIRDFDataSource> dataSource;
  GetOverlayDataSource(aChromeURL, getter_AddRefs(dataSource));

  if (dataSource)
  {
    nsCOMPtr<nsIRDFContainer> container;
    rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                            nsnull,
                                            NS_GET_IID(nsIRDFContainer),
                                            getter_AddRefs(container));
    if (NS_FAILED(rv))
      return NS_OK;
 
    char *lookup;
    aChromeURL->GetSpec(&lookup);

    // Get the chromeResource from this lookup string
    nsCOMPtr<nsIRDFResource> chromeResource;
    if (NS_FAILED(rv = GetResource(lookup, getter_AddRefs(chromeResource)))) {
        NS_ERROR("Unable to retrieve the resource corresponding to the chrome skin or content.");
        return rv;
    }
    nsAllocator::Free(lookup);

    if (NS_FAILED(container->Init(dataSource, chromeResource)))
      return NS_OK;

    nsCOMPtr<nsISimpleEnumerator> arcs;
    if (NS_FAILED(container->GetElements(getter_AddRefs(arcs))))
      return NS_OK;

    *aResult = new nsOverlayEnumerator(arcs);

    NS_ADDREF(*aResult);
  }

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::LoadDataSource(const nsCAutoString &aFileName, 
                                               nsIRDFDataSource **aResult, 
                                               PRBool aUseProfileDir)
{
  // Init the data source to null.
  *aResult = nsnull;

  nsCAutoString key;

  // Try the profile root first.
  if (aUseProfileDir) {
    key = mProfileRoot;
    key += aFileName;
  }
  else {
    key = mInstallRoot;
    key += aFileName;
  }

  if (mDataSourceTable)
  {
    nsStringKey skey(key);
    nsCOMPtr<nsISupports> supports = 
      getter_AddRefs(NS_STATIC_CAST(nsISupports*, mDataSourceTable->Get(&skey)));

    if (supports)
    {
      nsCOMPtr<nsIRDFDataSource> dataSource = do_QueryInterface(supports);
      if (dataSource)
      {
        *aResult = dataSource;
        NS_ADDREF(*aResult);
        return NS_OK;
      }
      return NS_ERROR_FAILURE;
    }
  }
    
  nsresult rv = nsComponentManager::CreateInstance(kRDFXMLDataSourceCID,
                                                     nsnull,
                                                     NS_GET_IID(nsIRDFDataSource),
                                                     (void**) aResult);
  if (NS_FAILED(rv)) return rv;

  nsCOMPtr<nsIRDFRemoteDataSource> remote = do_QueryInterface(*aResult);
  if (! remote)
      return NS_ERROR_UNEXPECTED;

  if (!mDataSourceTable)
    mDataSourceTable = new nsSupportsHashtable;

  // We need to read this synchronously.
  rv = remote->Init(key);
  if (NS_SUCCEEDED(rv))
    rv = remote->Refresh(PR_TRUE);
  
  nsCOMPtr<nsISupports> supports = do_QueryInterface(remote);
  nsStringKey skey(key);
  mDataSourceTable->Put(&skey, (void*)supports.get());

  return NS_OK;
}

////////////////////////////////////////////////////////////////////////////////

nsresult
nsChromeRegistry::GetResource(const nsCAutoString& aURL,
                              nsIRDFResource** aResult)
{
  nsresult rv = NS_OK;
  if (NS_FAILED(rv = mRDFService->GetResource(aURL, aResult))) {
    NS_ERROR("Unable to retrieve a resource for this URL.");
    *aResult = nsnull;
    return rv;
  }
  return NS_OK;
}

nsresult 
nsChromeRegistry::FollowArc(nsIRDFDataSource *aDataSource,
                            nsCString& aResult, 
                            nsIRDFResource* aChromeResource,
                            nsIRDFResource* aProperty)
{
  if (!aDataSource)
    return NS_ERROR_FAILURE;

  nsresult rv;

  nsCOMPtr<nsIRDFNode> chromeBase;
  if (NS_FAILED(rv = aDataSource->GetTarget(aChromeResource, aProperty, PR_TRUE, getter_AddRefs(chromeBase)))) {
    NS_ERROR("Unable to obtain a base resource.");
    return rv;
  }

  if (chromeBase == nsnull)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsIRDFResource> resource(do_QueryInterface(chromeBase));
  
  if (resource) {
    nsXPIDLCString uri;
    resource->GetValue( getter_Copies(uri) );
    aResult.Assign(uri);
    return NS_OK;
  }
 
  nsCOMPtr<nsIRDFLiteral> literal(do_QueryInterface(chromeBase));
  if (literal) {
    nsXPIDLString s;
    literal->GetValue( getter_Copies(s) );
    aResult.AssignWithConversion(s);
  }
  else {
    // This should _never_ happen.
    NS_ERROR("uh, this isn't a resource or a literal!");
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

////////////////////////////////////////////////////////////////////////

// theme stuff

NS_IMETHODIMP nsChromeRegistry::RefreshSkins()
{
  nsresult rv;

  // Flush the style sheet cache completely.
  // XXX For now flush everything.  need a better call that only flushes style sheets.
  NS_WITH_SERVICE(nsIXULPrototypeCache, xulCache, "component://netscape/rdf/xul-prototype-cache", &rv);
  if (NS_SUCCEEDED(rv) && xulCache) {
    xulCache->Flush();
  }
  
  // Flush the XBL docs.
  NS_WITH_SERVICE(nsIXBLService, xblService, "component://netscape/xbl", &rv);
  if (!xblService)
    return rv;
  xblService->FlushBindingDocuments();

  // Get the window mediator
  NS_WITH_SERVICE(nsIWindowMediator, windowMediator, kWindowMediatorCID, &rv);
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsISimpleEnumerator> windowEnumerator;

    if (NS_SUCCEEDED(windowMediator->GetEnumerator(nsnull, getter_AddRefs(windowEnumerator)))) {
      // Get each dom window
      PRBool more;
      windowEnumerator->HasMoreElements(&more);
      while (more) {
        nsCOMPtr<nsISupports> protoWindow;
        rv = windowEnumerator->GetNext(getter_AddRefs(protoWindow));
        if (NS_SUCCEEDED(rv) && protoWindow) {
          nsCOMPtr<nsIDOMWindow> domWindow = do_QueryInterface(protoWindow);
          if (domWindow)
            RefreshWindow(domWindow);
        }
        windowEnumerator->HasMoreElements(&more);
      }
    }
  }

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::RefreshWindow(nsIDOMWindow* aWindow)
{
  // Get the DOM document.
  nsCOMPtr<nsIDOMDocument> domDocument;
  aWindow->GetDocument(getter_AddRefs(domDocument));
  if (!domDocument)
    return NS_OK;

	nsCOMPtr<nsIDocument> document = do_QueryInterface(domDocument);
	if (!document)
	  return NS_OK;

  nsCOMPtr<nsIXULDocument> xulDoc = do_QueryInterface(domDocument);
  if (xulDoc) {
		
		nsCOMPtr<nsIHTMLContentContainer> container = do_QueryInterface(document);
		nsCOMPtr<nsICSSLoader> cssLoader;
		container->GetCSSLoader(*getter_AddRefs(cssLoader));

		// Build an array of nsIURIs of style sheets we need to load.
		nsCOMPtr<nsISupportsArray> urls;
		NS_NewISupportsArray(getter_AddRefs(urls));

		PRInt32 count = document->GetNumberOfStyleSheets();
  
		// Iterate over the style sheets.
		for (PRInt32 i = 0; i < count; i++) {
			// Get the style sheet
			nsCOMPtr<nsIStyleSheet> styleSheet = getter_AddRefs(document->GetStyleSheetAt(i));
    
			// Make sure we aren't the special style sheets that never change.  We
			// want to skip those.
			nsCOMPtr<nsIHTMLStyleSheet> attrSheet;
			container->GetAttributeStyleSheet(getter_AddRefs(attrSheet));

			nsCOMPtr<nsIHTMLCSSStyleSheet> inlineSheet;
			container->GetInlineStyleSheet(getter_AddRefs(inlineSheet));

			nsCOMPtr<nsIStyleSheet> attr = do_QueryInterface(attrSheet);
			nsCOMPtr<nsIStyleSheet> inl = do_QueryInterface(inlineSheet);
			if ((attr.get() != styleSheet.get()) &&
				(inl.get() != styleSheet.get())) {
				// Get the URI and add it to our array.
				nsCOMPtr<nsIURI> uri;
				styleSheet->GetURL(*getter_AddRefs(uri));
				urls->AppendElement(uri);
      
				// Remove the sheet. 
				count--;
				i--;
				document->RemoveStyleSheet(styleSheet);
			}
		}
  
		// Iterate over the URL array and kick off an asynchronous load of the
		// sheets for our doc.
		PRUint32 urlCount;
		urls->Count(&urlCount);
		for (PRUint32 j = 0; j < urlCount; j++) {
			nsCOMPtr<nsISupports> supports = getter_AddRefs(urls->ElementAt(j));
			nsCOMPtr<nsIURL> url = do_QueryInterface(supports);
			ProcessStyleSheet(url, cssLoader, document);
		}
	}

  return NS_OK;
}

NS_IMETHODIMP 
nsChromeRegistry::ProcessStyleSheet(nsIURL* aURL, nsICSSLoader* aLoader, nsIDocument* aDocument)
{
  PRBool doneLoading;
  nsresult rv = aLoader->LoadStyleLink(nsnull, // anElement
                                       aURL,
                                       nsAutoString(), // aTitle
                                       nsAutoString(), // aMedia
                                       kNameSpaceID_Unknown,
                                       aDocument->GetNumberOfStyleSheets(),
                                       nsnull,
                                       doneLoading,  // Ignore doneLoading. Don't care.
                                       nsnull);

  return rv;
}

NS_IMETHODIMP nsChromeRegistry::ReallyUpdateOverlayFromDataSource(char *aDocURI,
                                                                  const PRUnichar *aOverlayURI,
                                                                  PRBool aRemove)
{
  nsresult rv;
  nsCOMPtr<nsIURL> url;
  
  rv = nsComponentManager::CreateInstance("component://netscape/network/standard-url",
                                          nsnull,
                                          NS_GET_IID(nsIURL),
                                          getter_AddRefs(url));

  if (NS_FAILED(rv))
    return NS_OK;

  nsCAutoString str(aDocURI);
  url->SetSpec(str);
  nsCOMPtr<nsIRDFDataSource> dataSource;
  GetOverlayDataSource(url, getter_AddRefs(dataSource));

  if (!dataSource)
    return NS_OK;

  nsCOMPtr<nsIRDFResource> resource;
  rv = GetResource(str, getter_AddRefs(resource));

  if (NS_FAILED(rv))
    return NS_OK;

  nsCOMPtr<nsIRDFContainer> container;
  mRDFContainerUtils->MakeSeq(dataSource, resource, getter_AddRefs(container));
  if (!container) {
    // Already exists. Create a container instead.
    rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                      nsnull,
                                      NS_GET_IID(nsIRDFContainer),
                                      getter_AddRefs(container));
    container->Init(dataSource, resource);
  }

  nsAutoString unistr(aOverlayURI);
  nsCOMPtr<nsIRDFLiteral> literal;
  mRDFService->GetLiteral(unistr.GetUnicode(), getter_AddRefs(literal));

  if (aRemove)
    container->RemoveElement(literal, PR_TRUE);
  else {
    PRInt32 index;
    container->IndexOf(literal, &index);
    if (index == -1)
      container->AppendElement(literal);
  }

  nsCOMPtr<nsIRDFRemoteDataSource> remote = do_QueryInterface(dataSource, &rv);
  if (NS_FAILED(rv))
    return NS_OK;

  remote->Flush();

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::UpdateOverlay(nsIRDFDataSource *aDataSource, nsIRDFResource *aResource,
                                              PRBool aRemove)
{
  nsCOMPtr<nsIRDFContainer> container;
  nsresult rv;

  rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                          nsnull,
                                          NS_GET_IID(nsIRDFContainer),
                                          getter_AddRefs(container));
  if (NS_FAILED(rv))
    return NS_ERROR_FAILURE;
  
  if (NS_FAILED(container->Init(aDataSource, aResource)))
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(container->GetElements(getter_AddRefs(arcs))))
    return NS_ERROR_FAILURE;

  PRBool moreElements;
  arcs->HasMoreElements(&moreElements);

  char *value;
  aResource->GetValue(&value);

  while (moreElements)
  {
    nsCOMPtr<nsISupports> supports;
    arcs->GetNext(getter_AddRefs(supports));

    nsCOMPtr<nsIRDFLiteral> literal = do_QueryInterface(supports, &rv);

    if (NS_SUCCEEDED(rv))
    {
      const PRUnichar* valueStr;
      rv = literal->GetValueConst(&valueStr);
      if (NS_FAILED(rv))
        return rv;

      ReallyUpdateOverlayFromDataSource(value, valueStr, aRemove);
    }
    arcs->HasMoreElements(&moreElements);
  }
  nsAllocator::Free(value);

  return NS_OK;
}


NS_IMETHODIMP nsChromeRegistry::UpdateOverlays(nsIRDFDataSource *aDataSource,
                                               PRBool aRemove)
{
  nsresult rv;
  nsCOMPtr<nsIRDFResource> resource;
  rv = GetResource("urn:mozilla:overlays", getter_AddRefs(resource));

  if (!resource)
    return NS_OK;

  nsCOMPtr<nsIRDFContainer> container(do_CreateInstance("component://netscape/rdf/container"));
  if (!container)
    return NS_OK;

  if (NS_FAILED(container->Init(aDataSource, resource)))
    return NS_OK;

  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(container->GetElements(getter_AddRefs(arcs))))
    return NS_OK;

  PRBool moreElements;
  arcs->HasMoreElements(&moreElements);
  
  while (moreElements)
  {
    nsCOMPtr<nsISupports> supports;
    arcs->GetNext(getter_AddRefs(supports));

    nsCOMPtr<nsIRDFResource> resource = do_QueryInterface(supports, &rv);

    if (NS_SUCCEEDED(rv))
    {
      UpdateOverlay(aDataSource, resource, aRemove);
    }

    arcs->HasMoreElements(&moreElements);
  }

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::SelectSkin(const PRUnichar* aSkin,
                                        PRBool aUseProfile)
{
  return SetProvider("skin", mSelectedSkin, aSkin, aUseProfile, PR_TRUE);
}

NS_IMETHODIMP nsChromeRegistry::SelectLocale(const PRUnichar* aLocale,
                                          PRBool aUseProfile)
{
  return SetProvider("locale", mSelectedSkin, aLocale, aUseProfile, PR_TRUE);
}

NS_IMETHODIMP nsChromeRegistry::DeselectSkin(const PRUnichar* aSkin,
                                        PRBool aUseProfile)
{
  return SetProvider("skin", mSelectedSkin, aSkin, aUseProfile, PR_FALSE);
}

NS_IMETHODIMP nsChromeRegistry::DeselectLocale(const PRUnichar* aLocale,
                                          PRBool aUseProfile)
{
  return SetProvider("locale", mSelectedSkin, aLocale, aUseProfile, PR_FALSE);
}

NS_IMETHODIMP nsChromeRegistry::SetProvider(const nsCAutoString& aProvider,
                                            nsIRDFResource* aSelectionArc,
                                            const PRUnichar* aProviderName,
                                            PRBool aUseProfile,
                                            PRBool aIsAdding)
{
  // Build the provider resource str.
  // e.g., urn:mozilla:skin:aqua/1.0
  nsCAutoString resourceStr = "urn:mozilla:";
  resourceStr += aProvider;
  resourceStr += ":";
  resourceStr.AppendWithConversion(aProviderName);

  // Obtain the provider resource.
  nsresult rv = NS_OK;
  nsCOMPtr<nsIRDFResource> resource;
  rv = GetResource(resourceStr, getter_AddRefs(resource));
  if (NS_FAILED(rv)) {
    NS_ERROR("Unable to obtain the package resource.");
    return rv;
  }

  if (!resource)
    return NS_ERROR_FAILURE;

  // Follow the packages arc to the package resources.
  nsCOMPtr<nsIRDFNode> packageList;
  if (NS_FAILED(rv = mChromeDataSource->GetTarget(resource, mPackages, PR_TRUE, getter_AddRefs(packageList)))) {
    NS_ERROR("Unable to obtain the SEQ for the package list.");
    return rv;
  }
  
  if (!packageList)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsIRDFResource> packageSeq(do_QueryInterface(packageList));
  if (!packageSeq)
    return NS_ERROR_FAILURE;

  // Build an RDF container to wrap the SEQ
  nsCOMPtr<nsIRDFContainer> container;
  rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                          nsnull,
                                          NS_GET_IID(nsIRDFContainer),
                                          getter_AddRefs(container));
  if (NS_FAILED(rv))
    return NS_OK;

  if (NS_FAILED(container->Init(mChromeDataSource, packageSeq)))
    return NS_OK;

  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(container->GetElements(getter_AddRefs(arcs))))
    return NS_OK;

  // For each skin/package entry, follow the arcs to the real package
  // resource.
  PRBool more;
  arcs->HasMoreElements(&more);
  while (more) {
    nsCOMPtr<nsISupports> packageSkinEntry;
    rv = arcs->GetNext(getter_AddRefs(packageSkinEntry));
    if (NS_SUCCEEDED(rv) && packageSkinEntry) {
      nsCOMPtr<nsIRDFResource> entry = do_QueryInterface(packageSkinEntry);
      if (entry) {
         // Obtain the real package resource.
         nsCOMPtr<nsIRDFNode> packageNode;
         if (NS_FAILED(rv = mChromeDataSource->GetTarget(entry, mPackage, PR_TRUE, getter_AddRefs(packageNode)))) {
           NS_ERROR("Unable to obtain the package resource.");
           return rv;
         }

         // Select the skin for this package resource.
         nsCOMPtr<nsIRDFResource> packageResource(do_QueryInterface(packageNode));
         if (packageResource) {
           SetProviderForPackage(aProvider, packageResource, entry, aSelectionArc, aUseProfile, aIsAdding);
         }
      }
    }
    arcs->HasMoreElements(&more);
  }

  if(aProvider.Equals("skin")){
    LoadStyleSheet(getter_AddRefs(mScrollbarSheet), "chrome://global/skin/scrollbars.css"); 
  }
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::SetProviderForPackage(const nsCAutoString& aProvider,
                                        nsIRDFResource* aPackageResource, 
                                        nsIRDFResource* aProviderPackageResource, 
                                        nsIRDFResource* aSelectionArc, 
                                        PRBool aUseProfile, PRBool aIsAdding)
{
  // Figure out which file we're needing to modify, e.g., is it the install
  // dir or the profile dir, and get the right datasource.
  nsCAutoString dataSourceStr = "user-";
  dataSourceStr += aProvider;
  dataSourceStr += "s.rdf";
  
  nsCOMPtr<nsIRDFDataSource> dataSource;
  LoadDataSource(dataSourceStr, getter_AddRefs(dataSource), aUseProfile);
  if (!dataSource)
    return NS_ERROR_FAILURE;

  // Get the old targets
  nsCOMPtr<nsIRDFNode> retVal;
  dataSource->GetTarget(aPackageResource, aSelectionArc, PR_TRUE, getter_AddRefs(retVal));

  if (retVal) {
    if (aIsAdding) { 
      // Perform a CHANGE operation.
      dataSource->Change(aPackageResource, aSelectionArc, retVal, aProviderPackageResource);
    }
    else {
      // Only do an unassert if we are the current selected provider.
      nsCOMPtr<nsIRDFResource> res(do_QueryInterface(retVal));
      if (res.get() == aProviderPackageResource)
        dataSource->Unassert(aPackageResource, aSelectionArc, aProviderPackageResource);
    }
  } else if (aIsAdding) {
    // Do an ASSERT instead.
    dataSource->Assert(aPackageResource, aSelectionArc, aProviderPackageResource, PR_TRUE);
  }

  nsCOMPtr<nsIRDFRemoteDataSource> remote = do_QueryInterface(dataSource);
  if (!remote)
    return NS_ERROR_UNEXPECTED;

  remote->Flush();

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::SelectSkinForPackage(const PRUnichar *aSkin,
                                                  const PRUnichar *aPackageName,
                                                  PRBool aUseProfile)
{
  nsCAutoString provider("skin");
  return SelectProviderForPackage(provider, aSkin, aPackageName, mSelectedSkin, aUseProfile, PR_TRUE);
}

NS_IMETHODIMP nsChromeRegistry::SelectLocaleForPackage(const PRUnichar *aLocale,
                                                    const PRUnichar *aPackageName,
                                                    PRBool aUseProfile)
{
  nsCAutoString provider("locale");
  return SelectProviderForPackage(provider, aLocale, aPackageName, mSelectedLocale, aUseProfile, PR_TRUE);
}

NS_IMETHODIMP nsChromeRegistry::DeselectSkinForPackage(const PRUnichar *aSkin,
                                                  const PRUnichar *aPackageName,
                                                  PRBool aUseProfile)
{
  nsCAutoString provider("skin");
  return SelectProviderForPackage(provider, aSkin, aPackageName, mSelectedSkin, aUseProfile, PR_FALSE);
}

NS_IMETHODIMP nsChromeRegistry::DeselectLocaleForPackage(const PRUnichar *aLocale,
                                                    const PRUnichar *aPackageName,
                                                    PRBool aUseProfile)
{
  nsCAutoString provider("locale");
  return SelectProviderForPackage(provider, aLocale, aPackageName, mSelectedLocale, aUseProfile, PR_FALSE);
}

NS_IMETHODIMP nsChromeRegistry::SelectProviderForPackage(const nsCAutoString& aProviderType,
                                        const PRUnichar *aProviderName, 
                                        const PRUnichar *aPackageName, 
                                        nsIRDFResource* aSelectionArc, 
                                        PRBool aUseProfile, PRBool aIsAdding)
{
  nsCAutoString package = "urn:mozilla:package:";
  package.AppendWithConversion(aPackageName);

  nsCAutoString provider = "urn:mozilla:";
  provider += aProviderType;
  provider += ":";
  provider.AppendWithConversion(aProviderName);
  provider += ":";
  provider.AppendWithConversion(aPackageName);

  // Obtain the package resource.
  nsresult rv = NS_OK;
  nsCOMPtr<nsIRDFResource> packageResource;
  rv = GetResource(package, getter_AddRefs(packageResource));
  if (NS_FAILED(rv)) {
    NS_ERROR("Unable to obtain the package resource.");
    return rv;
  }

  if (!packageResource)
    return NS_ERROR_FAILURE;

  // Obtain the provider resource.
  nsCOMPtr<nsIRDFResource> providerResource;
  rv = GetResource(provider, getter_AddRefs(providerResource));
  if (NS_FAILED(rv)) {
    NS_ERROR("Unable to obtain the provider resource.");
    return rv;
  }

  if (!providerResource)
    return NS_ERROR_FAILURE;

  return SetProviderForPackage(aProviderType, packageResource, providerResource, aSelectionArc, aUseProfile, aIsAdding);;
}
   
NS_IMETHODIMP nsChromeRegistry::InstallProvider(const nsCAutoString& aProviderType,
                                                const nsCAutoString& aBaseURL,
                                                PRBool aUseProfile)
{
  // Load the data source found at the base URL.
  nsCOMPtr<nsIRDFDataSource> dataSource;
  nsresult rv = nsComponentManager::CreateInstance(kRDFXMLDataSourceCID,
                                                   nsnull,
                                                   NS_GET_IID(nsIRDFDataSource),
                                                   (void**) getter_AddRefs(dataSource));
  if (NS_FAILED(rv)) return rv;

  nsCOMPtr<nsIRDFRemoteDataSource> remote = do_QueryInterface(dataSource);
  if (!remote)
      return NS_ERROR_UNEXPECTED;

  // We need to read this synchronously.
  nsCAutoString key = aBaseURL;
  key += "manifest.rdf";
 
  rv = remote->Init(key);
  rv = remote->Refresh(PR_TRUE);

  if (NS_FAILED(rv))
    return NS_ERROR_FAILURE;

  // Load the data source that we wish to manipulate.
  nsCOMPtr<nsIRDFDataSource> installSource;
  nsCAutoString installStr = "all-";
  installStr += aProviderType;
  installStr += "s.rdf";
  LoadDataSource(installStr, getter_AddRefs(installSource), aUseProfile);
    
  if (!installSource)
    return NS_ERROR_FAILURE;

  // Build the prefix string. Only resources with this prefix string will have their
  // assertions copied.
  nsCAutoString prefix = "urn:mozilla:";
  prefix += aProviderType;
  prefix += ":";

  // Get all the resources
  nsCOMPtr<nsISimpleEnumerator> resources;
  dataSource->GetAllResources(getter_AddRefs(resources));

  // For each resource
  PRBool moreElements;
  resources->HasMoreElements(&moreElements);
  
  while (moreElements) {
    nsCOMPtr<nsISupports> supports;
    resources->GetNext(getter_AddRefs(supports));

    nsCOMPtr<nsIRDFResource> resource = do_QueryInterface(supports);
    
    // Check against the prefix string
    const char* value;
    resource->GetValueConst(&value);
    nsCAutoString val(value);
    if (val.Find(prefix) == 0) {
      // It's valid.
      
      if (aProviderType.Equals("package") && !val.Equals("urn:mozilla:package:root")) {
        // Get the literal for our base URL.
        nsAutoString unistr;unistr.AssignWithConversion(aBaseURL);
        nsCOMPtr<nsIRDFLiteral> literal;
        mRDFService->GetLiteral(unistr.GetUnicode(), getter_AddRefs(literal));

        nsCOMPtr<nsIRDFNode> retVal;
        installSource->GetTarget(resource, mBaseURL, PR_TRUE, getter_AddRefs(retVal));
        if (retVal)
          installSource->Change(resource, mBaseURL, retVal, literal);
        else
          installSource->Assert(resource, mBaseURL, literal, PR_TRUE);

        // install our overlays
        UpdateOverlays(dataSource, PR_FALSE);
      }

      nsCOMPtr<nsIRDFContainer> container;
      rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                            nsnull,
                                            NS_GET_IID(nsIRDFContainer),
                                            getter_AddRefs(container));
      if (NS_SUCCEEDED(container->Init(dataSource, resource))) {
        // XXX Deal with BAGS and ALTs? Aww, to hell with it. Who cares? I certainly don't.
        // We're a SEQ. Different rules apply. Do an AppendElement instead.
        // First do the decoration in the install data source.
        nsCOMPtr<nsIRDFContainer> installContainer;
        mRDFContainerUtils->MakeSeq(installSource, resource, getter_AddRefs(installContainer));
        if (!installContainer) {
          // Already exists. Create a container instead.
          rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                            nsnull,
                                            NS_GET_IID(nsIRDFContainer),
                                            getter_AddRefs(installContainer));
          installContainer->Init(installSource, resource);
        }

        // Put all our elements into the install container.
        nsCOMPtr<nsISimpleEnumerator> seqKids;
        container->GetElements(getter_AddRefs(seqKids));
        PRBool moreKids;
        seqKids->HasMoreElements(&moreKids);
        while (moreKids) {
          nsCOMPtr<nsISupports> supp;
          seqKids->GetNext(getter_AddRefs(supp));
          nsCOMPtr<nsIRDFNode> kid = do_QueryInterface(supp);
          PRInt32 index;
          installContainer->IndexOf(kid, &index);
          if (index == -1)
            installContainer->AppendElement(kid);
          seqKids->HasMoreElements(&moreKids);
        }

        // If we're a package, set up our base URL arc.
        

        // See if we're a packages seq.  If so, we need to set up the baseURL and
        // the package arcs.
        if (val.Find(":packages") != -1 && !aProviderType.Equals(nsCAutoString("package"))) {
          // Get the literal for our base URL.
          nsAutoString unistr;unistr.AssignWithConversion(aBaseURL);
          nsCOMPtr<nsIRDFLiteral> literal;
          mRDFService->GetLiteral(unistr.GetUnicode(), getter_AddRefs(literal));

          // Iterate over our kids a second time.
          nsCOMPtr<nsISimpleEnumerator> seqKids;
          container->GetElements(getter_AddRefs(seqKids));
          PRBool moreKids;
          seqKids->HasMoreElements(&moreKids);
          while (moreKids) {
            nsCOMPtr<nsISupports> supp;
            seqKids->GetNext(getter_AddRefs(supp));
            nsCOMPtr<nsIRDFResource> entry(do_QueryInterface(supp));
            if (entry) {
              nsCOMPtr<nsIRDFNode> retVal;
              installSource->GetTarget(entry, mBaseURL, PR_TRUE, getter_AddRefs(retVal));
              if (retVal)
                installSource->Change(entry, mBaseURL, retVal, literal);
              else
                installSource->Assert(entry, mBaseURL, literal, PR_TRUE);

              // Now set up the package arc.
              const char* val;
              entry->GetValueConst(&val);
              nsCAutoString value(val);
              PRInt32 index = value.RFind(":");
              if (index != -1) {
                // Peel off the package name.
                nsCAutoString packageName;
                value.Right(packageName, value.Length() - index - 1);

                nsCAutoString resourceName = "urn:mozilla:package:";
                resourceName += packageName;
                nsCOMPtr<nsIRDFResource> packageResource;
                GetResource(resourceName, getter_AddRefs(packageResource));
                if (packageResource) {
                  retVal = nsnull;
                  installSource->GetTarget(entry, mPackage, PR_TRUE, getter_AddRefs(retVal));
                  if (retVal)
                    installSource->Change(entry, mPackage, retVal, packageResource);
                  else
                    installSource->Assert(entry, mPackage, packageResource, PR_TRUE);
                }
              }
            }

            seqKids->HasMoreElements(&moreKids);
          }
        }
      }
      else {
        // We're not a seq. Get all of the arcs that go out.
        nsCOMPtr<nsISimpleEnumerator> arcs;
        dataSource->ArcLabelsOut(resource, getter_AddRefs(arcs));
      
        PRBool moreArcs;
        arcs->HasMoreElements(&moreArcs);
        while (moreArcs) {
          nsCOMPtr<nsISupports> supp;
          arcs->GetNext(getter_AddRefs(supp));
          nsCOMPtr<nsIRDFResource> arc = do_QueryInterface(supp);

          nsCOMPtr<nsIRDFNode> retVal;
          installSource->GetTarget(resource, arc, PR_TRUE, getter_AddRefs(retVal));
          nsCOMPtr<nsIRDFNode> newTarget;
          dataSource->GetTarget(resource, arc, PR_TRUE, getter_AddRefs(newTarget));
        
          if (retVal)
            installSource->Change(resource, arc, retVal, newTarget);
          else {
            // Do an ASSERT instead.
            installSource->Assert(resource, arc, newTarget, PR_TRUE);
          }
      
          arcs->HasMoreElements(&moreArcs);
        }
      }
    }
    resources->HasMoreElements(&moreElements);
  }
 
  // Flush the install source
  nsCOMPtr<nsIRDFRemoteDataSource> remoteInstall = do_QueryInterface(installSource, &rv);
  if (NS_FAILED(rv))
    return NS_OK;
  remoteInstall->Flush();

  // XXX Handle the installation of overlays.

  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::InstallSkin(const char* aBaseURL, PRBool aUseProfile)
{
  nsCAutoString provider("skin");
  return InstallProvider(provider, aBaseURL, aUseProfile);
}

NS_IMETHODIMP nsChromeRegistry::InstallLocale(const char* aBaseURL, PRBool aUseProfile)
{
  nsCAutoString provider("locale");
  return InstallProvider(provider, aBaseURL, aUseProfile);
}

NS_IMETHODIMP nsChromeRegistry::InstallPackage(const char* aBaseURL, PRBool aUseProfile)
{
  nsCAutoString provider("package");
  return InstallProvider(provider, aBaseURL, aUseProfile);
}

NS_IMETHODIMP nsChromeRegistry::UninstallSkin(const PRUnichar* aSkinName, PRBool aUseProfile)
{
  NS_ERROR("Write me!\n");
  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::UninstallLocale(const PRUnichar* aLocaleName, PRBool aUseProfile)
{
  NS_ERROR("Write me!\n");
  return NS_OK;
}

NS_IMETHODIMP nsChromeRegistry::UninstallPackage(const PRUnichar* aPackageName, PRBool aUseProfile)
{
  NS_ERROR("Write me!\n");
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::GetProfileRoot(nsCAutoString& aFileURL) 
{ 
  nsCOMPtr<nsIFileLocator> fl;
  
  nsresult rv = nsComponentManager::CreateInstance("component://netscape/filelocator",
                                          nsnull,
                                          NS_GET_IID(nsIFileLocator),
                                          getter_AddRefs(fl));

  if (NS_FAILED(rv))
    return NS_OK;

  // Build a fileSpec that points to the destination
  // (profile dir + chrome + package + provider + chrome.rdf)
  nsCOMPtr<nsIFileSpec> chromeFileInterface;
  fl->GetFileLocation(nsSpecialFileSpec::App_UserProfileDirectory50, getter_AddRefs(chromeFileInterface));

  if (chromeFileInterface) {
    nsFileSpec chromeFile;
    chromeFileInterface->GetFileSpec(&chromeFile);
    nsFileURL fileURL(chromeFile);
    const char* fileStr = fileURL.GetURLString();
    aFileURL = fileStr;
    aFileURL += "chrome/";
  }
  else return NS_ERROR_FAILURE;
  
  return NS_OK; 
}

NS_IMETHODIMP
nsChromeRegistry::GetInstallRoot(nsCAutoString& aFileURL) 
{ 
  nsCOMPtr<nsIFileLocator> fl;
  
  nsresult rv = nsComponentManager::CreateInstance("component://netscape/filelocator",
                                          nsnull,
                                          NS_GET_IID(nsIFileLocator),
                                          getter_AddRefs(fl));

  if (NS_FAILED(rv))
    return NS_OK;

  // Build a fileSpec that points to the destination
  // (profile dir + chrome + package + provider + chrome.rdf)
  nsCOMPtr<nsIFileSpec> chromeFileInterface;
  fl->GetFileLocation(nsSpecialFileSpec::App_ChromeDirectory, getter_AddRefs(chromeFileInterface));

  if (chromeFileInterface) {
    nsFileSpec chromeFile;
    chromeFileInterface->GetFileSpec(&chromeFile);
    nsFileURL fileURL(chromeFile);
    const char* fileStr = fileURL.GetURLString();
    aFileURL = fileStr;
  }
  else return NS_ERROR_FAILURE;
  
  return NS_OK; 
}

NS_IMETHODIMP
nsChromeRegistry::ReloadChrome()
{
	// Do a reload of all top level windows.
	nsresult rv;

  // Flush the cache completely.
  NS_WITH_SERVICE(nsIXULPrototypeCache, xulCache, "component://netscape/rdf/xul-prototype-cache", &rv);
  if (NS_SUCCEEDED(rv) && xulCache) {
    xulCache->Flush();
  }
  
  // Get the window mediator
  NS_WITH_SERVICE(nsIWindowMediator, windowMediator, kWindowMediatorCID, &rv);
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsISimpleEnumerator> windowEnumerator;

    if (NS_SUCCEEDED(windowMediator->GetEnumerator(nsnull, getter_AddRefs(windowEnumerator)))) {
      // Get each dom window
      PRBool more;
      windowEnumerator->HasMoreElements(&more);
      while (more) {
        nsCOMPtr<nsISupports> protoWindow;
        rv = windowEnumerator->GetNext(getter_AddRefs(protoWindow));
        if (NS_SUCCEEDED(rv) && protoWindow) {
          nsCOMPtr<nsPIDOMWindow> domWindow = do_QueryInterface(protoWindow);
          if (domWindow) {
						nsCOMPtr<nsIDOMLocation> location;
						domWindow->GetLocation(getter_AddRefs(location));
						if (location)
              location->Reload(PR_FALSE);
					}
        }
        windowEnumerator->HasMoreElements(&more);
      }
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::GetArcs(nsIRDFDataSource* aDataSource,
                          const nsCAutoString& aType,
                          nsISimpleEnumerator** aResult)
{
  nsCOMPtr<nsIRDFContainer> container;
  nsresult rv = nsComponentManager::CreateInstance("component://netscape/rdf/container",
                                          nsnull,
                                          NS_GET_IID(nsIRDFContainer),
                                          getter_AddRefs(container));
  if (NS_FAILED(rv))
    return NS_OK;

  nsCAutoString lookup("chrome:");
  lookup += aType;

  // Get the chromeResource from this lookup string
  nsCOMPtr<nsIRDFResource> chromeResource;
  if (NS_FAILED(rv = GetResource(lookup, getter_AddRefs(chromeResource)))) {
    NS_ERROR("Unable to retrieve the resource corresponding to the chrome skin or content.");
    return rv;
  }
  
  if (NS_FAILED(container->Init(aDataSource, chromeResource)))
    return NS_OK;

  nsCOMPtr<nsISimpleEnumerator> arcs;
  if (NS_FAILED(container->GetElements(getter_AddRefs(arcs))))
    return NS_OK;
  
  *aResult = arcs;
  NS_IF_ADDREF(*aResult);
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::AddToCompositeDataSource(PRBool aUseProfile)
{
  nsresult rv = NS_OK;
  if (!mChromeDataSource) {
    rv = nsComponentManager::CreateInstance("component://netscape/rdf/datasource?name=composite-datasource",
                                            nsnull,
                                            NS_GET_IID(nsIRDFCompositeDataSource),
                                            getter_AddRefs(mChromeDataSource));
    if (NS_FAILED(rv))
      return rv;

    // Also create and hold on to our UI data source.
    NS_NewChromeUIDataSource(mChromeDataSource, &mUIDataSource);
    mRDFService->RegisterDataSource(mUIDataSource, PR_FALSE);
  }
  
  if (aUseProfile) {
    // Profiles take precedence.  Load them first.
    nsCOMPtr<nsIRDFDataSource> dataSource;
    nsCAutoString name("user-skins.rdf");
    LoadDataSource(name, getter_AddRefs(dataSource), PR_TRUE);
    mChromeDataSource->AddDataSource(dataSource);

    name = "all-skins.rdf";
    LoadDataSource(name, getter_AddRefs(dataSource), PR_TRUE);
    mChromeDataSource->AddDataSource(dataSource);

    name = "user-locales.rdf";
    LoadDataSource(name, getter_AddRefs(dataSource), PR_TRUE);
    mChromeDataSource->AddDataSource(dataSource);

    name = "all-locales.rdf";
    LoadDataSource(name, getter_AddRefs(dataSource), PR_TRUE);
    mChromeDataSource->AddDataSource(dataSource);

    name = "all-packages.rdf";
    LoadDataSource(name, getter_AddRefs(dataSource), PR_TRUE);
    mChromeDataSource->AddDataSource(dataSource);
  }

  // Always load the install dir datasources
  nsCOMPtr<nsIRDFDataSource> dataSource;
  nsCAutoString name = "user-skins.rdf";
  LoadDataSource(name, getter_AddRefs(dataSource), PR_FALSE);
  mChromeDataSource->AddDataSource(dataSource);

  name = "all-skins.rdf";
  LoadDataSource(name, getter_AddRefs(dataSource), PR_FALSE);
  mChromeDataSource->AddDataSource(dataSource);

  name = "user-locales.rdf";
  LoadDataSource(name, getter_AddRefs(dataSource), PR_FALSE);
  mChromeDataSource->AddDataSource(dataSource);

  name = "all-locales.rdf";
  LoadDataSource(name, getter_AddRefs(dataSource), PR_FALSE);
  mChromeDataSource->AddDataSource(dataSource);

  name = "all-packages.rdf";
  LoadDataSource(name, getter_AddRefs(dataSource), PR_FALSE);
  mChromeDataSource->AddDataSource(dataSource);
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistry::GetBackstopSheets(nsISupportsArray **aResult)
{
  if (!mScrollbarSheet)
    LoadStyleSheet(getter_AddRefs(mScrollbarSheet), "chrome://global/skin/scrollbars.css"); 

  if(mScrollbarSheet || mUserSheet)
  {
    NS_NewISupportsArray(aResult);
    if(mScrollbarSheet)
      (*aResult)->AppendElement(mScrollbarSheet);
     
    if(mUserSheet)
      (*aResult)->AppendElement(mUserSheet);
  }
  return NS_OK;
}
                                    
void nsChromeRegistry::LoadStyleSheet(nsICSSStyleSheet** aSheet, const nsCString& aURL)
{
  // Load scrollbar style sheet
  nsresult rv; 

  nsCOMPtr<nsICSSLoader> loader;
  rv = nsComponentManager::CreateInstance(kCSSLoaderCID,
                                    nsnull,
                                    NS_GET_IID(nsICSSLoader),
                                    getter_AddRefs(loader));
  if(loader) {
    nsCOMPtr<nsIURL> url;
    rv = nsComponentManager::CreateInstance("component://netscape/network/standard-url",
                                    nsnull,
                                    NS_GET_IID(nsIURL),
                                    getter_AddRefs(url));
    if(url) {
      url->SetSpec(aURL);
      PRBool complete;
      rv = loader->LoadAgentSheet(url, *aSheet, complete,
                                 nsnull);
    }
  }
}

void nsChromeRegistry::GetUserSheetURL(nsCString & aURL)
{
  aURL = mProfileRoot;
  aURL.Append("user.css");
}

NS_IMETHODIMP
nsChromeRegistry::CheckForNewChrome() {

  nsresult rv;

  GetInstallRoot(mInstallRoot); // ensure install root is set

  nsCOMPtr<nsIFileLocator> locator;

  // open the installed-chrome file

  rv = nsComponentManager::CreateInstance("component://netscape/filelocator",
                                          nsnull,
                                          NS_GET_IID(nsIFileLocator),
                                          getter_AddRefs(locator));

  if (NS_FAILED(rv))
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsIFileSpec> listFileInterface;
  locator->GetFileLocation(nsSpecialFileSpec::App_ChromeDirectory, getter_AddRefs(listFileInterface));
  if (!listFileInterface)
    return NS_ERROR_FAILURE;

  nsFileSpec listFileSpec;
  nsCOMPtr<nsILocalFile> listFile;
  listFileInterface->GetFileSpec(&listFileSpec);
  NS_FileSpecToIFile(&listFileSpec, getter_AddRefs(listFile));

  PRFileDesc *file;

  listFile->AppendRelativePath(kInstalledChromeFileName);
  rv = listFile->OpenNSPRFileDesc(PR_RDWR, 0, &file);
  if (NS_FAILED(rv))
    return rv;

  // file is open. 

  PRFileInfo finfo;

  if (PR_GetOpenFileInfo(file, &finfo) == PR_SUCCESS) {
    char *dataBuffer = new char[finfo.size+1];
    if (dataBuffer) {
      PRInt32 bufferSize = PR_Read(file, dataBuffer, finfo.size);
      if (bufferSize > 0) {
        dataBuffer[bufferSize] = '\r'; //  be sure to terminate in a delimiter
        ProcessNewChromeBuffer(dataBuffer, bufferSize);
      }
      delete [] dataBuffer;
    }
  }
  PR_Close(file);
  listFile->Delete(PR_FALSE);

  return NS_OK;
}

// flaming unthreadsafe function
void
nsChromeRegistry::ProcessNewChromeBuffer(char *aBuffer, PRInt32 aLength) {

  char   *bufferEnd = aBuffer + aLength;
  char   *chromeType,      // "content", "locale" or "skin"
         *chromeProfile,   // "install" or "profile"
         *chromeLocType,   // type of location (local path or URL)
         *chromeLocation;  // base location of chrome (jar file)
  PRBool isProfile;

  nsCAutoString content("content");
  nsCAutoString locale("locale");
  nsCAutoString skin("skin");
  nsCAutoString profile("profile");

  static const char *delim = ",\r\n";

  chromeType = strtok(aBuffer, delim);
  do {
    // parse one line of installed-chrome.txt
    if (!chromeType)
      break;
    chromeProfile = strtok(0, delim);
    if (!chromeProfile || chromeProfile > bufferEnd)
      break;
    chromeLocType = strtok(0, delim); // unused for now. assume "path"
    if (!chromeLocType || chromeProfile > bufferEnd)
      break;
    chromeLocation = strtok(0, delim);
    if (!chromeLocation || chromeProfile > bufferEnd)
      break;

    isProfile = profile.Equals(chromeProfile);
    nsFileSpec chromeFile(chromeLocation);
    nsFileURL fileURL(chromeFile);
    const char* chromeURL = fileURL.GetURLString();

    // process the line
    if (skin.Equals(chromeType))
      InstallSkin(chromeURL, isProfile);
    else if (content.Equals(chromeType))
      InstallPackage(chromeURL, isProfile);
    else if (locale.Equals(chromeType))
      InstallLocale(chromeURL, isProfile);

    chromeType = strtok(0, delim);
  } while (chromeType < bufferEnd);
}

//////////////////////////////////////////////////////////////////////

nsresult
NS_NewChromeRegistry(nsIChromeRegistry** aResult)
{
    NS_PRECONDITION(aResult != nsnull, "null ptr");
    if (! aResult)
        return NS_ERROR_NULL_POINTER;

    nsChromeRegistry* chromeRegistry = new nsChromeRegistry();
    if (chromeRegistry == nsnull)
        return NS_ERROR_OUT_OF_MEMORY;
    NS_ADDREF(chromeRegistry);
    *aResult = chromeRegistry;
    return NS_OK;
}
