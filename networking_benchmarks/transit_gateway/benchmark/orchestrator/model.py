"""The stack under test.

Identity (instance ids, bucket, group, port) comes from the CloudFormation outputs. Per-instance
facts (private IP, AZ, AMI, type) are read from the instance's own metadata service during prepare
and kept in the manifest, so a run needs no EC2 or CloudFormation calls.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict, List, Optional

from .aws import aws


@dataclass(frozen=True)
class Instance:
    id: str
    az: str
    private_ip: str
    ami: str
    itype: str
    public_ip: Optional[str]


@dataclass
class Outputs:
    stack_id: str
    region: str
    bucket: str
    group: str
    port: str
    publisher_id: str
    subscriber_ids: List[str]

    @property
    def all_ids(self) -> List[str]:
        return [self.publisher_id, *self.subscriber_ids]


@dataclass
class Stack:
    name: str
    stack_id: str
    region: str
    bucket: str
    group: str
    port: str
    publisher: Instance
    subscribers: List[Instance]

    @property
    def all(self) -> List[Instance]:
        return [self.publisher, *self.subscribers]

    @property
    def all_ids(self) -> List[str]:
        return [i.id for i in self.all]


def load_outputs(name: str) -> Outputs:
    """One CloudFormation call."""
    doc = json.loads(aws("cloudformation", "describe-stacks", "--stack-name", name, "--output", "json"))
    st = doc["Stacks"][0]
    o: Dict[str, str] = {x["OutputKey"]: x["OutputValue"] for x in st.get("Outputs", [])}
    return Outputs(stack_id=st["StackId"], region=st["StackId"].split(":")[3], bucket=o.get("S3BucketName", ""),
                   group=o.get("MulticastGroup", ""), port=o.get("MulticastPort", ""),
                   publisher_id=o["PublisherInstanceId"],
                   subscriber_ids=[s for s in o["SubscriberInstanceIds"].split(",") if s])


def public_ips(ids: List[str]) -> Dict[str, Optional[str]]:
    """Public IPs from EC2. Only the SSH transport needs them, and it needs them before it can
    reach the instances to ask for anything else."""
    text = aws("ec2", "describe-instances", "--instance-ids", *ids, "--query",
               "Reservations[].Instances[].[InstanceId,PublicIpAddress]", "--output", "text")
    out: Dict[str, Optional[str]] = {i: None for i in ids}
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) >= 2 and f[1] not in ("", "None"):
            out[f[0]] = f[1]
    return out


def stack_from_manifest(name: str, manifest: Dict[str, Any]) -> Stack:
    s, inst = manifest["stack"], manifest["instances"]

    def make(iid: str) -> Instance:
        f = inst[iid]
        return Instance(iid, f["az"], f["private_ip"], f["ami"], f["itype"], f.get("public_ip"))

    return Stack(name=name, stack_id=s["stack_id"], region=s["region"], bucket=s["bucket"], group=s["group"],
                 port=s["port"], publisher=make(s["publisher"]), subscribers=[make(i) for i in s["subscribers"]])
