Blocktwitter spam is excluded from transaction traces, but still present
in block data. You can filter it out by suffix in packed_trx string:


```
   "block" : {
      "action_mroot" : "12000ED6A0503E84F6D52DAB346D7A72984E1781D7439154CFFC254E9151AC51",
      "block_extensions" : [],
      "confirmed" : "0",
      "header_extensions" : [],
      "new_producers" : null,
      "previous" : "00A100E3BA6A2A023039182FB490E0E92A6C4E210940A77C65D6D509712FFAD6",
      "producer" : "eosbeijingbp",
      "producer_signature" : "SIG_K1_K6YRFp4VxepqY6Bh7a46jdtdXY2nF7cMbg3qZgDRLyKWmrzfQRUZsr5qrBhz4CXu6accDdE3GVS39JEroGAgDvPpU2S4Ho",
      "schedule_version" : "267",
      "timestamp" : "2018-08-11T07:27:51.000",
      "transaction_mroot" : "466C1047EF5FB167161A6DF6F8BEC69A08BC57CD971EAD6D7383F7149BD920A5",
      "transactions" : [
         {
            "cpu_usage_us" : "286",
            "net_usage_words" : "13",
            "status" : "0",
            "trx" : {
               "compression" : "0",
               "packed_context_free_data" : "",
               "packed_trx" : "70946E5B99FFFBEA8FDA00000000017055CE8E6788683C0000000080AC14CF017055CE8EE7E94C4300000000A8ED32320B0A5745204C4F564520424D00",
               "signatures" : [
                  "SIG_K1_KY89LQKkdscG1QK46ZDPnkeRfRBXNF2NfugvZ8Rx3i14dgvEyXM3DRN44Q3fmMWJASUzM364qYv3nsZwPHQf2mHEfrVpw4"
               ]
            }
         },
         {
            "cpu_usage_us" : "294",
            "net_usage_words" : "13",
            "status" : "0",
            "trx" : {
               "compression" : "0",
               "packed_context_free_data" : "",
               "packed_trx" : "6F946E5B99FFFBEA8FDA00000000017055CE8E6788683C0000000080AC14CF017055CE8EE7E94C4300000000A8ED32320B0A5745204C4F564520424D00",
               "signatures" : [
                  "SIG_K1_K5x9kRSX9VkVZLsEGuJjkcirznXftgTP9eCnVPSFsauGSRCvV87tig4e6YkMuWjGYkTGPmGtEt7tUL7MxAh1A9KYkCrwyj"
               ]
            }
         },
         {
            "cpu_usage_us" : "263",
            "net_usage_words" : "13",
            "status" : "0",
            "trx" : {
               "compression" : "0",
               "packed_context_free_data" : "",
               "packed_trx" : "6E946E5B99FFFBEA8FDA00000000017055CE8E6788683C0000000080AC14CF017055CE8EE7E94C4300000000A8ED32320B0A5745204C4F564520424D00",
               "signatures" : [
                  "SIG_K1_KASMaFvQkft7SquCzW1J3m7fEcqeoiHK9nKwC546c54CxarmuQmM1N6R8YkLZBM1nPwVP9LtRk9eiRS8PdfDYREzb3cLDr"
               ]
            }
         },

```