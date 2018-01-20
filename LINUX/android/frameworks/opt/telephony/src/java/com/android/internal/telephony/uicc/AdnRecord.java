/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.telephony.uicc;

import android.os.Parcel;
import android.os.Parcelable;
import android.telephony.PhoneNumberUtils;
import android.text.TextUtils;
import android.telephony.Rlog;

import java.util.Arrays;


/**
 *
 * Used to load or store ADNs (Abbreviated Dialing Numbers).
 *
 * {@hide}
 *
 */
public class AdnRecord implements Parcelable {
    static final String LOG_TAG = "AdnRecord";

    //***** Instance Variables

    String mAlphaTag = null;
    String mNumber = null;
    String[] mEmails;
    String[] mAdditionalNumbers = null;
    int mExtRecord = 0xff;
    int mEfid;                   // or 0 if none
    int mRecordNumber;           // or 0 if none


    //***** Constants

    // In an ADN record, everything but the alpha identifier
    // is in a footer that's 14 bytes
    static final int FOOTER_SIZE_BYTES = 14;

    // Maximum size of the un-extended number field
    static final int MAX_NUMBER_SIZE_BYTES = 11;

    static final int EXT_RECORD_LENGTH_BYTES = 13;
    static final int EXT_RECORD_TYPE_ADDITIONAL_DATA = 2;
    static final int EXT_RECORD_TYPE_MASK = 3;
    static final int MAX_EXT_CALLED_PARTY_LENGTH = 0xa;

    // ADN offset
    static final int ADN_BCD_NUMBER_LENGTH = 0;
    static final int ADN_TON_AND_NPI = 1;
    static final int ADN_DIALING_NUMBER_START = 2;
    static final int ADN_DIALING_NUMBER_END = 11;
    static final int ADN_CAPABILITY_ID = 12;
    static final int ADN_EXTENSION_ID = 13;

    //***** Static Methods

    public static final Parcelable.Creator<AdnRecord> CREATOR
            = new Parcelable.Creator<AdnRecord>() {
        @Override
        public AdnRecord createFromParcel(Parcel source) {
            int efid;
            int recordNumber;
            String alphaTag;
            String number;
            String[] emails;
            String[] additionalNumbers;
            efid = source.readInt();
            recordNumber = source.readInt();
            alphaTag = source.readString();
            number = source.readString();
            emails = source.readStringArray();
            additionalNumbers = source.readStringArray();

            return new AdnRecord(efid, recordNumber, alphaTag, number, emails, additionalNumbers);
        }

        @Override
        public AdnRecord[] newArray(int size) {
            return new AdnRecord[size];
        }
    };


    //***** Constructor
    public AdnRecord (byte[] record) {
        this(0, 0, record);
    }

    public AdnRecord (int efid, int recordNumber, byte[] record) {
        this.mEfid = efid;
        this.mRecordNumber = recordNumber;
        parseRecord(record);
    }

    public AdnRecord (String alphaTag, String number) {
        this(0, 0, alphaTag, number);
    }

    public AdnRecord (String alphaTag, String number, String[] emails) {
        this(0, 0, alphaTag, number, emails);
    }

    public AdnRecord(String alphaTag, String number, String[] emails, String[] additionalNumbers) {
        this(0, 0, alphaTag, number, emails, additionalNumbers);
    }

    public AdnRecord (int efid, int recordNumber, String alphaTag, String number, String[] emails) {
        this.mEfid = efid;
        this.mRecordNumber = recordNumber;
        this.mAlphaTag = alphaTag;
        this.mNumber = number;
        this.mEmails = emails;
        this.mAdditionalNumbers = null;
    }

    public AdnRecord(int efid, int recordNumber, String alphaTag, String number, String[] emails,
            String[] additionalNumbers) {
        this.mEfid = efid;
        this.mRecordNumber = recordNumber;
        this.mAlphaTag = alphaTag;
        this.mNumber = number;
        this.mEmails = emails;
        this.mAdditionalNumbers = additionalNumbers;
    }

    public AdnRecord(int efid, int recordNumber, String alphaTag, String number) {
        this.mEfid = efid;
        this.mRecordNumber = recordNumber;
        this.mAlphaTag = alphaTag;
        this.mNumber = number;
        this.mEmails = null;
        this.mAdditionalNumbers = null;
    }

    //***** Instance Methods

    public String getAlphaTag() {
        return mAlphaTag;
    }

    public String getNumber() {
        return mNumber;
    }

    public String[] getEmails() {
        return mEmails;
    }

    public void setEmails(String[] emails) {
        this.mEmails = emails;
    }

    public String[] getAdditionalNumbers() {
        return mAdditionalNumbers;
    }

    public void setAdditionalNumbers(String[] additionalNumbers) {
        this.mAdditionalNumbers = additionalNumbers;
    }

    @Override
    public String toString() {
        return "ADN Record 'Tag:" + mAlphaTag + "', Num:'" + mNumber + ", Emails:" +
            Arrays.toString(mEmails) + ", Anrs:" + Arrays.toString(mAdditionalNumbers) + "'";
    }

    public boolean isEmpty() {
        return TextUtils.isEmpty(mAlphaTag) && TextUtils.isEmpty(mNumber) && mEmails == null
                && mAdditionalNumbers == null;
    }

    public boolean hasExtendedRecord() {
        return mExtRecord != 0 && mExtRecord != 0xff;
    }

    /** Helper function for {@link #isEqual}. */
    private static boolean stringCompareNullEqualsEmpty(String s1, String s2) {
        if (s1 == s2) {
            return true;
        }
        if (s1 == null) {
            s1 = "";
        }
        if (s2 == null) {
            s2 = "";
        }
        return (s1.equals(s2));
    }

    /** Help function for ANR/EMAIL array compare. */
    private static boolean arrayCompareNullEqualsEmpty(String s1[], String s2[]) {
        if (s1 == s2) {
            return true;
        }

        if (s1 == null) {
            s1 = new String[1];
            s1[0] = "";
        }

        if (s2 == null) {
            s2 = new String[1];
            s2[0] = "";
        }

        for (String str:s1) {
            if (TextUtils.isEmpty(str)) {
                continue;
            } else {
                if (Arrays.asList(s2).contains(str)) {
                    continue;
                } else {
                    return false;
                }
            }
        }

        for (String str:s2) {
            if (TextUtils.isEmpty(str)) {
                continue;
            } else {
                if (Arrays.asList(s1).contains(str)) {
                    continue;
                } else {
                    return false;
                }
            }
        }

        return true;
    }

    public boolean isEqual(AdnRecord adn) {
        return ( stringCompareNullEqualsEmpty(mAlphaTag, adn.mAlphaTag) &&
                stringCompareNullEqualsEmpty(mNumber, adn.mNumber) &&
                arrayCompareNullEqualsEmpty(mEmails, adn.mEmails)
                && arrayCompareNullEqualsEmpty(mAdditionalNumbers, adn.mAdditionalNumbers));
    }

    public String[] updateAnrEmailArrayHelper(String dest[], String src[], int fileCount) {
        if (fileCount == 0) {
            return null;
        }

        // delete insert scenario
        if (dest == null || src == null) {
            return dest;
        }

        String[] ref = new String[fileCount];
        for (int i = 0; i < fileCount; i++) {
            ref[i] = "";
        }

        // Find common elements and put in the ref
        // To save SIM_IO
        for (int i = 0; i < src.length; i++) {
            if (TextUtils.isEmpty(src[i])) {
                continue;
            }
            for (int j = 0; j < dest.length; j++) {
                if (src[i].equals(dest[j])) {
                    ref[i] = src[i];
                    break;
                }
            }
        }

        // fill out none common element into the ""
        for (int i = 0; i < dest.length; i++) {
            if (Arrays.asList(ref).contains(dest[i])) {
                continue;
            } else {
                for (int j = 0; j < ref.length; j++) {
                    if (TextUtils.isEmpty(ref[j])) {
                        ref[j] = dest[i];
                        break;
                    }
                }
            }
        }
        return ref;
    }

    public void updateAnrEmailArray(AdnRecord adn, int emailFileNum, int anrFileNum) {
        mEmails = updateAnrEmailArrayHelper(mEmails, adn.mEmails, emailFileNum);
        mAdditionalNumbers = updateAnrEmailArrayHelper(mAdditionalNumbers,
                    adn.mAdditionalNumbers, anrFileNum);
    }
    //***** Parcelable Implementation

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mEfid);
        dest.writeInt(mRecordNumber);
        dest.writeString(mAlphaTag);
        dest.writeString(mNumber);
        dest.writeStringArray(mEmails);
        dest.writeStringArray(mAdditionalNumbers);
    }

    /**
     * Build adn hex byte array based on record size
     * The format of byte array is defined in 51.011 10.5.1
     *
     * @param recordSize is the size X of EF record
     * @return hex byte[recordSize] to be written to EF record
     *          return null for wrong format of dialing number or tag
     */
    public byte[] buildAdnString(int recordSize) {
        byte[] bcdNumber;
        byte[] byteTag;
        byte[] adnString;
        int length;
        int footerOffset = recordSize - FOOTER_SIZE_BYTES;

        // create an empty record
        adnString = new byte[recordSize];
        for (int i = 0; i < recordSize; i++) {
            adnString[i] = (byte) 0xFF;
        }

        if ((TextUtils.isEmpty(mNumber)) && (TextUtils.isEmpty(mAlphaTag))) {
            Rlog.w(LOG_TAG, "[buildAdnString] Empty dialing number");
            return adnString;   // return the empty record (for delete)
        } else if (mAlphaTag != null && mAlphaTag.length() > footerOffset) {
            Rlog.w(LOG_TAG,
                    "[buildAdnString] Max length of tag is " + footerOffset);
            return null;
        } else if (mNumber != null && mNumber.length()
                > (ADN_DIALING_NUMBER_END - ADN_DIALING_NUMBER_START + 1) * 4) {
            Rlog.w(LOG_TAG,
                    "[buildAdnString] Max length of dialing number is 40");
            return null;
        } else {
            if (!(TextUtils.isEmpty(mNumber))) {
                bcdNumber = PhoneNumberUtils.numberToCalledPartyBCD(mNumber);

            if (mNumber != null && mNumber.length()
                    > (ADN_DIALING_NUMBER_END - ADN_DIALING_NUMBER_START + 1) * 2
                    && mNumber.length() <= 40 ) {
                if (!hasExtendedRecord()) {
                    Rlog.d(LOG_TAG,
                            "[buildAdnString] No EXT1 file/record exists");
                    return null;
                }
                length = (ADN_DIALING_NUMBER_END - ADN_DIALING_NUMBER_START + 1);
            } else {
                length = (bcdNumber.length);
            }

            System.arraycopy(bcdNumber, 0, adnString,
                    footerOffset + ADN_TON_AND_NPI,
                    length);

            adnString[footerOffset + ADN_BCD_NUMBER_LENGTH] =
                    (byte) length;
            adnString[footerOffset + ADN_CAPABILITY_ID]
                    = (byte) 0xFF; // Capability Id
            adnString[footerOffset + ADN_EXTENSION_ID]
                    = (byte) mExtRecord; // Extension Record Id

        }
        if (!TextUtils.isEmpty(mAlphaTag)) {
            byteTag = IccUtils.stringToAdnStringField(mAlphaTag);
            System.arraycopy(byteTag, 0, adnString, 0, byteTag.length);
        }
        return adnString;
        }
    }

    /* Build the EXT1 data to store the remaining digits
    /* when number length is greater than 20 */
    public byte[] buildExtData() {
        byte[] extData;
        byte[] extendedNum;

        // Each EXT1 record is 13 bytes.
        extData = new byte[EXT_RECORD_LENGTH_BYTES];
        for (int i = 0; i <  EXT_RECORD_LENGTH_BYTES; i++) {
            extData[i] = (byte) 0xFF;
        }

        extendedNum = PhoneNumberUtils.numberToCalledPartyBCD(mNumber);

        // extData stores the remaining digits of the number greater than 20.
        System.arraycopy(extendedNum, 10, extData, 2, (extendedNum.length - 10));
        extData[0] = (byte) 2; // Record Type: Additional data
        extData[1] = (byte) ((extendedNum.length - 10)); //Length of extension data in bytes.
        return extData;

    }

    /**
     * See TS 51.011 10.5.10
     */
    public void
    appendExtRecord (byte[] extRecord) {
        try {
            if (extRecord.length != EXT_RECORD_LENGTH_BYTES) {
                return;
            }

            if ((extRecord[0] & EXT_RECORD_TYPE_MASK)
                    != EXT_RECORD_TYPE_ADDITIONAL_DATA) {
                return;
            }

            if ((0xff & extRecord[1]) > MAX_EXT_CALLED_PARTY_LENGTH) {
                // invalid or empty record
                return;
            }

            mNumber += PhoneNumberUtils.calledPartyBCDFragmentToString(
                                        extRecord, 2, 0xff & extRecord[1]);

            // We don't support ext record chaining.

        } catch (RuntimeException ex) {
            Rlog.w(LOG_TAG, "Error parsing AdnRecord ext record", ex);
        }
    }

    //***** Private Methods

    /**
     * alphaTag and number are set to null on invalid format
     */
    private void
    parseRecord(byte[] record) {
        try {
            mAlphaTag = IccUtils.adnStringFieldToString(
                            record, 0, record.length - FOOTER_SIZE_BYTES);

            int footerOffset = record.length - FOOTER_SIZE_BYTES;

            int numberLength = 0xff & record[footerOffset];

            if (numberLength > MAX_NUMBER_SIZE_BYTES) {
                // Invalid number length
                mNumber = "";
                return;
            }

            // Please note 51.011 10.5.1:
            //
            // "If the Dialling Number/SSC String does not contain
            // a dialling number, e.g. a control string deactivating
            // a service, the TON/NPI byte shall be set to 'FF' by
            // the ME (see note 2)."

            mNumber = PhoneNumberUtils.calledPartyBCDToString(
                            record, footerOffset + 1, numberLength);


            mExtRecord = 0xff & record[record.length - 1];

            mEmails = null;
            mAdditionalNumbers = null;

        } catch (RuntimeException ex) {
            Rlog.w(LOG_TAG, "Error parsing AdnRecord", ex);
            mNumber = "";
            mAlphaTag = "";
            mEmails = null;
            mAdditionalNumbers = null;
        }
    }

    public String[] getAnrNumbers() {
        return getAdditionalNumbers();
    }
}