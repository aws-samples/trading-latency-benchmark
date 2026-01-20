import com.aws.trading.ExchangeProtocolImpl;

import java.util.UUID;

public class ConfigTest {
    public static void main(String[] args) {
        String instrument = "BTC/USDT";
        var stringBuilder = new StringBuilder();
        for(int i = 0; i< 300; i++){
            stringBuilder.append(instrument);
        }
        System.out.println(new ExchangeProtocolImpl().createBuyOrder(stringBuilder.toString(), UUID.randomUUID().toString()).content().readableBytes());
        System.out.println(stringBuilder.toString());
    }
}
