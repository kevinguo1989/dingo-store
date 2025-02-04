/*
 * Copyright 2021, Zetyun DataPortal All rights reserved.
 */

package io.dingodb.sdk.common.index;

import lombok.AllArgsConstructor;
import lombok.Getter;
import lombok.ToString;

@Getter
@ToString
@AllArgsConstructor
public class IndexParameter {

    private IndexType indexType;
    private AbstractIndexParameter vectorIndexParameter;
    private AbstractIndexParameter scalarIndexParameter;

    public enum IndexType {
        INDEX_TYPE_NONE,
        INDEX_TYPE_VECTOR,
        INDEX_TYPE_SCALAR
    }

}
