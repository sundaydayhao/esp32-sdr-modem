package cn.sdrstudio.app;
import org.junit.*;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.json.*;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class) @Config(sdk=34)
public class ReceiverContractTest {
    @Test public void switchModesRestoresAudioAndUsesRealWireEnums() throws Exception {
        JSONObject old=ReceiverConfig.defaults();old.put("mode",9);old.put("enabled",false);old.put("monitor",false);old.put("frequency_hz",433920000);
        JSONObject fm=ReceiverConfig.select(old,3,0);assertTrue(fm.getBoolean("enabled"));assertTrue(fm.getBoolean("monitor"));assertEquals(106200000,fm.getLong("frequency_hz"));
        fm.put("frequency_hz",89800000);assertEquals(89800000,ReceiverConfig.select(fm,3,0).getLong("frequency_hz"));
        JSONObject adsb=ReceiverConfig.select(fm,6,0);assertEquals(1090000000,adsb.getLong("frequency_hz"));assertFalse(adsb.getBoolean("monitor"));assertFalse(adsb.getBoolean("decode"));
        JSONObject sub=ReceiverConfig.select(fm,8,0);assertEquals(433920000,sub.getLong("frequency_hz"));assertEquals(433050000,sub.getLong("scan_start_hz"));
        JSONObject scan=ReceiverConfig.select(sub,7,0);assertEquals(433050000,scan.getLong("scan_start_hz"));assertFalse(scan.getBoolean("monitor"));
        assertThrows(IllegalArgumentException.class,()->ReceiverConfig.select(fm,10,0));
        assertEquals(9,old.getInt("mode")); // edits do not mutate the previous device snapshot
    }
    @Test public void inputAndInterlockValidation() throws Exception {
        assertEquals(433920000,ReceiverConfig.mhz("433.920000"));
        for(String value:new String[]{"NaN","Infinity","-1","2001",""})assertThrows(IllegalArgumentException.class,()->ReceiverConfig.mhz(value));
        assertEquals("http://192.168.4.1",DeviceApi.validateAddress("192.168.4.1"));
        for(String address:new String[]{"https://192.168.4.1","example.com","8.8.8.8","192.168.4.1/path","192.168.4.1?x=1","192.168.4.999","user@192.168.4.1"})assertThrows(IllegalArgumentException.class,()->DeviceApi.validateAddress(address));
        assertTrue(ReceiverConfig.busy(new JSONObject("{\"record_saving\":true}")));assertTrue(ReceiverConfig.busy(new JSONObject("{\"replaying\":true}")));assertFalse(ReceiverConfig.busy(new JSONObject()));
    }
    @Test public void mapCoordinatesAndStalePlanes() throws Exception {
        assertEquals(22.5431,MapProjection.latitude(MapProjection.y(22.5431)),.000001);
        assertEquals(2/360.0,MapProjection.deltaX(-179,179),.000001);
        assertTrue(MapProjection.metersPerPixel(22,15)<MapProjection.metersPerPixel(22,10));
        JSONObject plane=new JSONObject("{\"has_position\":true,\"latitude\":22.6,\"longitude\":114.0,\"position_age_ms\":200}");
        assertTrue(AircraftMapView.valid(plane));plane.put("position_age_ms",70000);assertFalse(AircraftMapView.valid(plane));
        plane.put("position_age_ms",200);plane.put("has_position",false);assertFalse(AircraftMapView.valid(plane));
    }
}
