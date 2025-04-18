/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/HTMLTableSectionElement.h"
#include "mozilla/GenericSpecifiedValuesInlines.h"
#include "nsMappedAttributes.h"
#include "nsAttrValueInlines.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/HTMLTableSectionElementBinding.h"
#include "nsContentUtils.h"

NS_IMPL_NS_NEW_HTML_ELEMENT(TableSection)

namespace mozilla {
namespace dom {

// you will see the phrases "rowgroup" and "section" used interchangably

HTMLTableSectionElement::~HTMLTableSectionElement()
{
}

JSObject*
HTMLTableSectionElement::WrapNode(JSContext *aCx, JS::Handle<JSObject*> aGivenProto)
{
  return HTMLTableSectionElementBinding::Wrap(aCx, this, aGivenProto);
}

NS_IMPL_CYCLE_COLLECTION_CLASS(HTMLTableSectionElement)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(HTMLTableSectionElement,
                                                  nsGenericHTMLElement)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRows)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(HTMLTableSectionElement,
                                               nsGenericHTMLElement)

NS_IMPL_ELEMENT_CLONE(HTMLTableSectionElement)

nsIHTMLCollection*
HTMLTableSectionElement::Rows()
{
  if (!mRows) {
    mRows = new nsContentList(this,
                              mNodeInfo->NamespaceID(),
                              nsGkAtoms::tr,
                              nsGkAtoms::tr,
                              false);
  }

  return mRows;
}

already_AddRefed<nsGenericHTMLElement>
HTMLTableSectionElement::InsertRow(int32_t aIndex, ErrorResult& aError)
{
  if (aIndex < -1) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  nsIHTMLCollection* rows = Rows();

  uint32_t rowCount = rows->Length();
  if (aIndex > (int32_t)rowCount) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  bool doInsert = (aIndex < int32_t(rowCount)) && (aIndex != -1);

  // create the row
  RefPtr<mozilla::dom::NodeInfo> nodeInfo;
  nsContentUtils::QNameChanged(mNodeInfo, nsGkAtoms::tr,
                               getter_AddRefs(nodeInfo));

  RefPtr<nsGenericHTMLElement> rowContent =
    NS_NewHTMLTableRowElement(nodeInfo.forget());
  if (!rowContent) {
    aError.Throw(NS_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }

  if (doInsert) {
    nsCOMPtr<nsINode> refNode = rows->Item(aIndex);
    nsINode::InsertBefore(*rowContent, refNode, aError);
  } else {
    nsINode::AppendChild(*rowContent, aError);
  }
  return rowContent.forget();
}

void
HTMLTableSectionElement::DeleteRow(int32_t aValue, ErrorResult& aError)
{
  if (aValue < -1) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  nsIHTMLCollection* rows = Rows();

  uint32_t refIndex;
  if (aValue == -1) {
    refIndex = rows->Length();
    if (refIndex == 0) {
      return;
    }

    --refIndex;
  }
  else {
    refIndex = (uint32_t)aValue;
  }

  nsINode* row = rows->Item(refIndex);
  if (!row) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  nsINode::RemoveChild(*row, aError);
}

bool
HTMLTableSectionElement::ParseAttribute(int32_t aNamespaceID,
                                        nsAtom* aAttribute,
                                        const nsAString& aValue,
                                        nsIPrincipal* aMaybeScriptedPrincipal,
                                        nsAttrValue& aResult)
{
  if (aNamespaceID == kNameSpaceID_None) {
    /* ignore these attributes, stored simply as strings
       ch
    */
    if (aAttribute == nsGkAtoms::charoff) {
      return aResult.ParseIntWithBounds(aValue, 0);
    }
    if (aAttribute == nsGkAtoms::height) {
      return aResult.ParseSpecialIntValue(aValue);
    }
    if (aAttribute == nsGkAtoms::align) {
      return ParseTableCellHAlignValue(aValue, aResult);
    }
    if (aAttribute == nsGkAtoms::bgcolor) {
      return aResult.ParseColor(aValue);
    }
    if (aAttribute == nsGkAtoms::valign) {
      return ParseTableVAlignValue(aValue, aResult);
    }
  }

  return nsGenericHTMLElement::ParseBackgroundAttribute(aNamespaceID,
                                                        aAttribute, aValue,
                                                        aResult) ||
         nsGenericHTMLElement::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                              aMaybeScriptedPrincipal, aResult);
}

void
HTMLTableSectionElement::MapAttributesIntoRule(const nsMappedAttributes* aAttributes,
                                               GenericSpecifiedValues* aData)
{
  // height: value
  if (!aData->PropertyIsSet(eCSSProperty_height)) {
    const nsAttrValue* value = aAttributes->GetAttr(nsGkAtoms::height);
    if (value && value->Type() == nsAttrValue::eInteger)
      aData->SetPixelValue(eCSSProperty_height, (float)value->GetIntegerValue());
  }
  nsGenericHTMLElement::MapDivAlignAttributeInto(aAttributes, aData);
  nsGenericHTMLElement::MapVAlignAttributeInto(aAttributes, aData);
  nsGenericHTMLElement::MapBackgroundAttributesInto(aAttributes, aData);
  nsGenericHTMLElement::MapCommonAttributesInto(aAttributes, aData);
}

NS_IMETHODIMP_(bool)
HTMLTableSectionElement::IsAttributeMapped(const nsAtom* aAttribute) const
{
  static const MappedAttributeEntry attributes[] = {
    { &nsGkAtoms::align },
    { &nsGkAtoms::valign },
    { &nsGkAtoms::height },
    { nullptr }
  };

  static const MappedAttributeEntry* const map[] = {
    attributes,
    sCommonAttributeMap,
    sBackgroundAttributeMap,
  };

  return FindAttributeDependence(aAttribute, map);
}


nsMapRuleToAttributesFunc
HTMLTableSectionElement::GetAttributeMappingFunction() const
{
  return &MapAttributesIntoRule;
}

} // namespace dom
} // namespace mozilla
