/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: MIT-0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
package com.aws.trading;

import java.util.Arrays;

public class StringMath {

    /**
    * The idea here is to multiply up the double by the scale, round it, then split it where the
    * decimal place should be. Where there are multiple leading zeros we fill these in and return
    * the string. Why not use BigDecimal you cry... because its shit.
     * Why not use the much simpler test version below? well it's twice as slow.
    * @param qty the double to be converted to string.
    * @param QTY_MULTIPLIER essentially 10 ^ SCALE
    * @param SCALE the number of supported decimal places in the qty or price.
    * @return A decimal string which is supported by the exchange.
    * */
    public static String QtyToString(double qty, int SCALE, long QTY_MULTIPLIER){
        final long qty_l = Math.round(qty * QTY_MULTIPLIER);
        final char[] qty_new = Long.toString(qty_l).toCharArray();
        int len = qty_new.length;
        final char[] out;
        if(len > SCALE){
            out = new char[len + 1];
            final int loc = len - SCALE;
            System.arraycopy(qty_new, 0,out,0,loc);
            out[loc] = '.';
            System.arraycopy(qty_new, loc,out,loc+1,len-loc);
        } else {
            out = new char[SCALE + 2];
            out[0] = '0';out[1]='.';
            final int loc = SCALE - len;
            if(loc > 0){
                Arrays.fill(out,2,loc+2,'0');
            }
            System.arraycopy(qty_new, 0,out,loc+2,len);
        }
        return new String(out);
    }

    public static String QtyToStringTest(double qty, int SCALE, double QTY_MULTIPLIER){
        return Double.toString(Math.round(qty * QTY_MULTIPLIER)/QTY_MULTIPLIER);
    }

    public static long doubleToLong(final String input){
        return Long.parseLong(input.replace(".",""));
    }
}
