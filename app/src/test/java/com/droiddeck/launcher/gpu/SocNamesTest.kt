package com.droiddeck.launcher.gpu

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SocNamesTest {
    private fun props(vararg pairs: Pair<String, String>): (String) -> String = { pairs.toMap()[it].orEmpty() }

    @Test fun theModelIsFoundWhereVendorsPutIt() {
        // AYANEO Pocket FIT: ro.soc.model blank, the vendor property set, ro.fota.platform a branch name.
        assertEquals("SG8350P", SocNames.model(props("ro.vendor.qti.soc_model" to "SG8350P", "ro.fota.platform" to "MSM_13.0")))
        assertEquals("QCS8550", SocNames.model(props("ro.soc.model" to "QCS8550", "ro.vendor.qti.soc_model" to "other")))
        assertEquals("", SocNames.model(props("ro.fota.platform" to "MSM_13.0")))
    }

    @Test fun knownModelsAndPlatformsGetTheirNames() {
        assertEquals("Snapdragon 8 Gen 2", SocNames.name("QCS8550", ""))
        assertEquals("Snapdragon 8 Gen 3 family", SocNames.name("SG8350P", "pineapple"))
        assertNull(SocNames.name("XYZ123", "nowhere"))
        assertEquals("Snapdragon 8 Gen 2 (QCS8550)", SocNames.label("QCS8550", ""))
        assertEquals("Snapdragon 8 Gen 3 family (SG8350P)", SocNames.label("SG8350P", "pineapple"))
        assertEquals("XYZ123", SocNames.label("XYZ123", ""))
    }

    @Test fun tensorsAreNamedAsChipsNotFamilies() {
        assertEquals("Tensor G1", SocNames.name("GS101", "gs101"))
        assertEquals("Tensor G2", SocNames.name("", "gs201"))
        assertEquals("Tensor G3", SocNames.name("", "zuma"))
        assertEquals("Tensor G4", SocNames.name("", "zumapro"))
        assertEquals("Tensor G4 (GS401)", SocNames.label("GS401", "zumapro"))
        // A platform the table does not know still gets no name: nothing is inferred from digits.
        assertNull(SocNames.name("XYZ999", "nowhere"))
    }
}
